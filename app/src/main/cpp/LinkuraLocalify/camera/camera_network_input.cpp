#include "camera_controller_bindings.hpp"
#include "camera_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>

namespace L4Camera {
    namespace {
        struct NetworkCameraInputState {
            float leftStickX = 0.0f;
            float leftStickY = 0.0f;
            float rightStickX = 0.0f;
            float rightStickY = 0.0f;
            float leftTrigger = 0.0f;
            float leftGrip = 0.0f;
            float rightTrigger = 0.0f;
            float rightGrip = 0.0f;
            float orientationX = 0.0f;
            float orientationY = 0.0f;
            float orientationZ = 0.0f;
            float orientationW = 1.0f;
            float hmdPosX = 0.0f;
            float hmdPosY = 0.0f;
            float hmdPosZ = 0.0f;
            float baselineHmdPosX = 0.0f;
            float baselineHmdPosY = 0.0f;
            float baselineHmdPosZ = 0.0f;
            float appliedHmdOffsetX = 0.0f;
            float appliedHmdOffsetY = 0.0f;
            float appliedHmdOffsetZ = 0.0f;
            float baselineOrientationX = 0.0f;
            float baselineOrientationY = 0.0f;
            float baselineOrientationZ = 0.0f;
            float baselineOrientationW = 1.0f;
            float smoothedYawOffsetDegrees = 0.0f;
            float smoothedPitchOffsetDegrees = 0.0f;
            float smoothedRollOffsetDegrees = 0.0f;
            float appliedYawOffsetDegrees = 0.0f;
            float appliedPitchOffsetDegrees = 0.0f;
            float appliedRollOffsetDegrees = 0.0f;
            float ipdMeters = 0.064f;
            float hmdVerticalFovDegrees = 90.0f;
            float leftEyeAngleLeftRadians = -0.7853982f;
            float leftEyeAngleRightRadians = 0.7853982f;
            float leftEyeAngleUpRadians = 0.7853982f;
            float leftEyeAngleDownRadians = -0.7853982f;
            float rightEyeAngleLeftRadians = -0.7853982f;
            float rightEyeAngleRightRadians = 0.7853982f;
            float rightEyeAngleUpRadians = 0.7853982f;
            float rightEyeAngleDownRadians = -0.7853982f;
            bool hasHmdOrigin = false;
            bool hasHeadOrientationOrigin = false;
            uint32_t buttons = 0;
            uint32_t prevButtons = 0;
            uint32_t flags = 0;
        };

        struct QuaternionInput {
            float x;
            float y;
            float z;
            float w;
        };

        struct Vector3Input {
            float x;
            float y;
            float z;
        };

        NetworkCameraInputState networkCameraInputState;
        std::mutex networkCameraInputMutex;
        constexpr uint32_t networkInputFlagResetBaseline = 1u << 1;

        float clampAbs(float value, float maxAbs) {
            return std::clamp(value, -maxAbs, maxAbs);
        }

        float radToDeg(float value) {
            return value * 57.2957795f;
        }

        QuaternionInput normalizeQuaternion(QuaternionInput value) {
            const float lengthSquared =
                (value.x * value.x) + (value.y * value.y) + (value.z * value.z) + (value.w * value.w);
            if (!std::isfinite(lengthSquared) || lengthSquared <= 0.000001f) {
                return {0.0f, 0.0f, 0.0f, 1.0f};
            }

            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            return {
                value.x * inverseLength,
                value.y * inverseLength,
                value.z * inverseLength,
                value.w * inverseLength
            };
        }

        QuaternionInput conjugateQuaternion(const QuaternionInput& value) {
            return {-value.x, -value.y, -value.z, value.w};
        }

        QuaternionInput multiplyQuaternion(const QuaternionInput& lhs, const QuaternionInput& rhs) {
            return {
                (lhs.w * rhs.x) + (lhs.x * rhs.w) + (lhs.y * rhs.z) - (lhs.z * rhs.y),
                (lhs.w * rhs.y) - (lhs.x * rhs.z) + (lhs.y * rhs.w) + (lhs.z * rhs.x),
                (lhs.w * rhs.z) + (lhs.x * rhs.y) - (lhs.y * rhs.x) + (lhs.z * rhs.w),
                (lhs.w * rhs.w) - (lhs.x * rhs.x) - (lhs.y * rhs.y) - (lhs.z * rhs.z)
            };
        }

        Vector3Input rotateVector(const QuaternionInput& rotation, const Vector3Input& value) {
            const QuaternionInput vectorQuaternion{value.x, value.y, value.z, 0.0f};
            const auto rotated = multiplyQuaternion(
                multiplyQuaternion(rotation, vectorQuaternion),
                conjugateQuaternion(rotation)
            );
            return {rotated.x, rotated.y, rotated.z};
        }

        float vectorLength(const Vector3Input& value) {
            return std::sqrt((value.x * value.x) + (value.y * value.y) + (value.z * value.z));
        }

        Vector3Input normalizeVector(const Vector3Input& value) {
            const float length = vectorLength(value);
            if (!std::isfinite(length) || length <= 0.000001f) {
                return {0.0f, 0.0f, 0.0f};
            }

            return {value.x / length, value.y / length, value.z / length};
        }

        Vector3Input crossVector(const Vector3Input& lhs, const Vector3Input& rhs) {
            return {
                (lhs.y * rhs.z) - (lhs.z * rhs.y),
                (lhs.z * rhs.x) - (lhs.x * rhs.z),
                (lhs.x * rhs.y) - (lhs.y * rhs.x)
            };
        }

        float dotVector(const Vector3Input& lhs, const Vector3Input& rhs) {
            return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
        }

        Vector3Input convertOpenXrVectorToUnity(const Vector3Input& value) {
            return {value.x, value.y, -value.z};
        }

        Vector3Input rejectVectorOnAxis(const Vector3Input& value, const Vector3Input& axis) {
            const auto normalizedAxis = normalizeVector(axis);
            return {
                value.x - (normalizedAxis.x * dotVector(value, normalizedAxis)),
                value.y - (normalizedAxis.y * dotVector(value, normalizedAxis)),
                value.z - (normalizedAxis.z * dotVector(value, normalizedAxis))
            };
        }

        float signedAngleDegreesAroundAxis(
            const Vector3Input& from,
            const Vector3Input& to,
            const Vector3Input& axis
        ) {
            const auto fromNormalized = normalizeVector(from);
            const auto toNormalized = normalizeVector(to);
            const auto axisNormalized = normalizeVector(axis);
            if (vectorLength(fromNormalized) <= 0.000001f
                || vectorLength(toNormalized) <= 0.000001f
                || vectorLength(axisNormalized) <= 0.000001f) {
                return 0.0f;
            }

            const auto cross = crossVector(fromNormalized, toNormalized);
            const auto sinValue = dotVector(axisNormalized, cross);
            const auto cosValue = std::clamp(dotVector(fromNormalized, toNormalized), -1.0f, 1.0f);
            return radToDeg(std::atan2(sinValue, cosValue));
        }

        void calculateHeadRelativeAnglesDegrees(
            const QuaternionInput& baseline,
            const QuaternionInput& current,
            float& yawDegrees,
            float& pitchDegrees,
            float& rollDegrees
        ) {
            const auto baselineForward = normalizeVector(convertOpenXrVectorToUnity(
                rotateVector(normalizeQuaternion(baseline), {0.0f, 0.0f, -1.0f})
            ));
            const auto baselineUp = normalizeVector(convertOpenXrVectorToUnity(
                rotateVector(normalizeQuaternion(baseline), {0.0f, 1.0f, 0.0f})
            ));
            const auto currentForward = normalizeVector(convertOpenXrVectorToUnity(
                rotateVector(normalizeQuaternion(current), {0.0f, 0.0f, -1.0f})
            ));
            const auto currentUp = normalizeVector(convertOpenXrVectorToUnity(
                rotateVector(normalizeQuaternion(current), {0.0f, 1.0f, 0.0f})
            ));

            const auto worldUp = Vector3Input{0.0f, 1.0f, 0.0f};
            const auto baselineHorizontalForward = rejectVectorOnAxis(baselineForward, worldUp);
            const auto currentHorizontalForward = rejectVectorOnAxis(currentForward, worldUp);
            yawDegrees = signedAngleDegreesAroundAxis(
                baselineHorizontalForward,
                currentHorizontalForward,
                worldUp
            );

            const auto baselinePitchDegrees =
                radToDeg(std::asin(std::clamp(baselineForward.y, -1.0f, 1.0f)));
            const auto currentPitchDegrees =
                radToDeg(std::asin(std::clamp(currentForward.y, -1.0f, 1.0f)));
            pitchDegrees = currentPitchDegrees - baselinePitchDegrees;

            const auto baselineRollUp = rejectVectorOnAxis(baselineUp, currentForward);
            const auto currentRollUp = rejectVectorOnAxis(currentUp, currentForward);
            rollDegrees = signedAngleDegreesAroundAxis(
                baselineRollUp,
                currentRollUp,
                currentForward
            );

            if (!std::isfinite(yawDegrees)) yawDegrees = 0.0f;
            if (!std::isfinite(pitchDegrees)) pitchDegrees = 0.0f;
            if (!std::isfinite(rollDegrees)) rollDegrees = 0.0f;
        }

        void cameraRawInputThread() {
            using namespace BaseCamera;

            std::thread([]() {
                if (cameraMoveState.threadRunning) return;
                cameraMoveState.threadRunning = true;
                bool rightTriggerCharacterSwitchLatch = false;
                bool rightGripCharacterSwitchLatch = false;

                while (true) {
                    NetworkCameraInputState networkInputSnapshot;
                    {
                        std::lock_guard<std::mutex> lock(networkCameraInputMutex);
                        networkInputSnapshot = networkCameraInputState;
                    }

                    if (networkInputSnapshot.hmdVerticalFovDegrees >= 20.0f
                        && networkInputSnapshot.hmdVerticalFovDegrees <= 170.0f) {
                        baseCamera.fov = networkInputSnapshot.hmdVerticalFovDegrees;
                    }

                    if (cameraMoveState.w) camera_forward(LinkuraLocal::Config::cameraMovementSensitivity);
                    if (cameraMoveState.s) camera_back(LinkuraLocal::Config::cameraMovementSensitivity);
                    if (cameraMoveState.a) camera_left(LinkuraLocal::Config::cameraMovementSensitivity);
                    if (cameraMoveState.d) camera_right(LinkuraLocal::Config::cameraMovementSensitivity);
                    if (cameraMoveState.ctrl) camera_down(LinkuraLocal::Config::cameraVerticalSensitivity);
                    if (cameraMoveState.space) camera_up(LinkuraLocal::Config::cameraVerticalSensitivity);
                    if (cameraMoveState.up) cameraLookat_up(moveAngel);
                    if (cameraMoveState.down) cameraLookat_down(moveAngel);
                    if (cameraMoveState.left) cameraLookat_left(moveAngel);
                    if (cameraMoveState.right) cameraLookat_right(moveAngel);
                    if (cameraMoveState.q) changeCameraFOV(0.5f * LinkuraLocal::Config::cameraFovSensitivity);
                    if (cameraMoveState.e) changeCameraFOV(-0.5f * LinkuraLocal::Config::cameraFovSensitivity);
                    if (cameraMoveState.r) L4Camera::baseCamera.setCamera(&L4Camera::originCamera);
                    if (cameraMoveState.i) ChangeLiveFollowCameraOffsetY(offsetMoveStep);
                    if (cameraMoveState.k) ChangeLiveFollowCameraOffsetY(-offsetMoveStep);
                    if (cameraMoveState.j) ChangeLiveFollowCameraOffsetX(0.8f);
                    if (cameraMoveState.l) ChangeLiveFollowCameraOffsetX(-0.8f);
                    if (std::abs(cameraMoveState.thumb_l_right) > 0.1f) JLThumbRight(cameraMoveState.thumb_l_right);
                    if (std::abs(cameraMoveState.thumb_l_down) > 0.1f) JLThumbDown(cameraMoveState.thumb_l_down);
                    if (std::abs(cameraMoveState.thumb_r_right) > 0.1f) JRThumbRight(cameraMoveState.thumb_r_right);
                    if (std::abs(cameraMoveState.thumb_r_down) > 0.1f) JRThumbDown(cameraMoveState.thumb_r_down);
                    if (std::abs(cameraMoveState.lt_button) > 0.1f) {
                        camera_down(
                            cameraMoveState.lt_button * l_sensitivity
                            * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60
                        );
                    }
                    if (std::abs(cameraMoveState.rt_button) > 0.1f) {
                        camera_up(
                            cameraMoveState.rt_button * l_sensitivity
                            * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60
                        );
                    }
                    if (cameraMoveState.lb_button) {
                        changeCameraFOV(0.5f * r_sensitivity * LinkuraLocal::Config::cameraFovSensitivity);
                    }
                    if (cameraMoveState.rb_button) {
                        changeCameraFOV(-0.5f * r_sensitivity * LinkuraLocal::Config::cameraFovSensitivity);
                    }
                    if (cameraMoveState.dpad_up) JDadUp();
                    if (cameraMoveState.dpad_left) JDadLeft();
                    if (cameraMoveState.dpad_right) JDadRight();

                    if (std::abs(networkInputSnapshot.leftStickX) > 0.01f) {
                        camera_right(
                            networkInputSnapshot.leftStickX * l_sensitivity
                            * LinkuraLocal::Config::cameraMovementSensitivity * baseCamera.fov / 60
                        );
                    }
                    if (std::abs(networkInputSnapshot.leftStickY) > 0.01f) {
                        camera_forward(
                            networkInputSnapshot.leftStickY * l_sensitivity
                            * LinkuraLocal::Config::cameraMovementSensitivity * baseCamera.fov / 60
                        );
                    }
                    if (std::abs(networkInputSnapshot.rightStickX) > 0.01f) {
                        JRThumbRight(networkInputSnapshot.rightStickX);
                    }
                    if (std::abs(networkInputSnapshot.rightStickY) > 0.01f) {
                        JRThumbDown(-networkInputSnapshot.rightStickY);
                    }
                    if (networkInputSnapshot.leftTrigger > 0.01f) {
                        camera_up(
                            networkInputSnapshot.leftTrigger * l_sensitivity
                            * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60
                        );
                    }
                    if (networkInputSnapshot.leftGrip > 0.01f) {
                        camera_down(
                            networkInputSnapshot.leftGrip * l_sensitivity
                            * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60
                        );
                    }

                    constexpr float hmdRotationGain = 1.6f;
                    constexpr float hmdRollGain = 1.0f;
                    constexpr float yprSmoothing = 0.35f;
                    constexpr float yprDeadzoneDegrees = 0.12f;
                    if (networkInputSnapshot.hasHeadOrientationOrigin) {
                        float relativeYawDegrees = 0.0f;
                        float relativePitchDegrees = 0.0f;
                        float relativeRollDegrees = 0.0f;
                        calculateHeadRelativeAnglesDegrees(
                            {
                                networkInputSnapshot.baselineOrientationX,
                                networkInputSnapshot.baselineOrientationY,
                                networkInputSnapshot.baselineOrientationZ,
                                networkInputSnapshot.baselineOrientationW
                            },
                            {
                                networkInputSnapshot.orientationX,
                                networkInputSnapshot.orientationY,
                                networkInputSnapshot.orientationZ,
                                networkInputSnapshot.orientationW
                            },
                            relativeYawDegrees,
                            relativePitchDegrees,
                            relativeRollDegrees
                        );

                        auto desiredYawOffsetDegrees = relativeYawDegrees * hmdRotationGain;
                        auto desiredPitchOffsetDegrees = relativePitchDegrees * hmdRotationGain;
                        auto desiredRollOffsetDegrees = relativeRollDegrees * hmdRollGain;

                        desiredPitchOffsetDegrees = clampAbs(desiredPitchOffsetDegrees, 75.0f);
                        desiredRollOffsetDegrees = clampAbs(desiredRollOffsetDegrees, 75.0f);

                        auto smoothedYawOffsetDegrees =
                            networkInputSnapshot.smoothedYawOffsetDegrees
                            + (desiredYawOffsetDegrees - networkInputSnapshot.smoothedYawOffsetDegrees) * yprSmoothing;
                        auto smoothedPitchOffsetDegrees =
                            networkInputSnapshot.smoothedPitchOffsetDegrees
                            + (desiredPitchOffsetDegrees - networkInputSnapshot.smoothedPitchOffsetDegrees) * yprSmoothing;
                        auto smoothedRollOffsetDegrees =
                            networkInputSnapshot.smoothedRollOffsetDegrees
                            + (desiredRollOffsetDegrees - networkInputSnapshot.smoothedRollOffsetDegrees) * yprSmoothing;

                        if (std::abs(smoothedYawOffsetDegrees) < yprDeadzoneDegrees) smoothedYawOffsetDegrees = 0.0f;
                        if (std::abs(smoothedPitchOffsetDegrees) < yprDeadzoneDegrees) smoothedPitchOffsetDegrees = 0.0f;
                        if (std::abs(smoothedRollOffsetDegrees) < yprDeadzoneDegrees) smoothedRollOffsetDegrees = 0.0f;

                        const auto yawDeltaDegrees =
                            smoothedYawOffsetDegrees - networkInputSnapshot.appliedYawOffsetDegrees;
                        const auto pitchDeltaDegrees =
                            smoothedPitchOffsetDegrees - networkInputSnapshot.appliedPitchOffsetDegrees;

                        if (std::abs(yawDeltaDegrees) > 0.001f) {
                            cameraLookat_right(yawDeltaDegrees / LinkuraLocal::Config::cameraRotationSensitivity);
                        }
                        if (std::abs(pitchDeltaDegrees) > 0.001f) {
                            cameraLookat_down(-pitchDeltaDegrees / LinkuraLocal::Config::cameraRotationSensitivity);
                        }

                        {
                            std::lock_guard<std::mutex> lock(networkCameraInputMutex);
                            networkCameraInputState.smoothedYawOffsetDegrees = smoothedYawOffsetDegrees;
                            networkCameraInputState.smoothedPitchOffsetDegrees = smoothedPitchOffsetDegrees;
                            networkCameraInputState.smoothedRollOffsetDegrees = smoothedRollOffsetDegrees;
                            networkCameraInputState.appliedYawOffsetDegrees = smoothedYawOffsetDegrees;
                            networkCameraInputState.appliedPitchOffsetDegrees = smoothedPitchOffsetDegrees;
                            networkCameraInputState.appliedRollOffsetDegrees = smoothedRollOffsetDegrees;
                        }
                    } else {
                        std::lock_guard<std::mutex> lock(networkCameraInputMutex);
                        networkCameraInputState.baselineOrientationX = networkInputSnapshot.orientationX;
                        networkCameraInputState.baselineOrientationY = networkInputSnapshot.orientationY;
                        networkCameraInputState.baselineOrientationZ = networkInputSnapshot.orientationZ;
                        networkCameraInputState.baselineOrientationW = networkInputSnapshot.orientationW;
                        networkCameraInputState.smoothedYawOffsetDegrees = 0.0f;
                        networkCameraInputState.smoothedPitchOffsetDegrees = 0.0f;
                        networkCameraInputState.smoothedRollOffsetDegrees = 0.0f;
                        networkCameraInputState.appliedYawOffsetDegrees = 0.0f;
                        networkCameraInputState.appliedPitchOffsetDegrees = 0.0f;
                        networkCameraInputState.appliedRollOffsetDegrees = 0.0f;
                        networkCameraInputState.hasHeadOrientationOrigin = true;
                    }

                    const auto currentButtons = networkInputSnapshot.buttons;
                    const auto previousButtons = networkInputSnapshot.prevButtons;
                    const auto risingButtons = (~previousButtons) & currentButtons;
                    {
                        std::lock_guard<std::mutex> lock(networkCameraInputMutex);
                        networkCameraInputState.prevButtons = currentButtons;
                    }

                    constexpr uint32_t buttonA = (1u << 0);
                    constexpr uint32_t buttonB = (1u << 1);
                    constexpr uint32_t buttonX = (1u << 2);
                    constexpr uint32_t buttonY = (1u << 3);

                    SyncNetworkControllerNavigationStateFromRuntime();

                    if (risingButtons & buttonA) {
                        CycleNetworkControllerMajorView();
                    }
                    if (risingButtons & buttonB) {
                        CycleCurrentCharacterCameraType();
                    }
                    if (risingButtons & buttonX) {
                        // Reserved for future "ActionA" toggle.
                        // ToggleActionA();
                    }
                    if (risingButtons & buttonY) {
                        ToggleCharacterCameraManualLook();
                    }

                    if (risingButtons & (1u << 4)) {
                        if (GetCameraMode() == CameraMode::FREE) {
                            baseCamera.setCamera(&originCamera);
                        }
                        firstPersonPosOffset = {0, 0.064f, 0.000f};
                        followPosOffset = {0, 0, 1.5f};
                        followLookAtOffset = {0, 0};
                    }
                    if (risingButtons & (1u << 5)) {
                        reset_camera();
                    }

                    const bool isRightTriggerPressed = networkInputSnapshot.rightTrigger >= 0.6f;
                    const bool isRightGripPressed = networkInputSnapshot.rightGrip >= 0.6f;
                    if (isRightTriggerPressed && !rightTriggerCharacterSwitchLatch) {
                        SelectNextCharacterFromController();
                    }
                    if (isRightGripPressed && !rightGripCharacterSwitchLatch) {
                        SelectPreviousCharacterFromController();
                    }
                    rightTriggerCharacterSwitchLatch = isRightTriggerPressed;
                    rightGripCharacterSwitchLatch = isRightGripPressed;

                    ApplyNetworkControllerNavigationState();

                    constexpr float hmdPosGain = 24.0f;
                    constexpr float hmdPosDeadzone = 0.00005f;
                    const float horizontalMoveScale =
                        hmdPosGain * l_sensitivity * LinkuraLocal::Config::cameraMovementSensitivity * baseCamera.fov / 60;
                    const float verticalMoveScale =
                        hmdPosGain * l_sensitivity * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60;
                    if (networkInputSnapshot.hasHmdOrigin) {
                        const float targetOffsetX =
                            (networkInputSnapshot.hmdPosX - networkInputSnapshot.baselineHmdPosX) * horizontalMoveScale;
                        const float targetOffsetY =
                            (networkInputSnapshot.hmdPosY - networkInputSnapshot.baselineHmdPosY) * verticalMoveScale;
                        const float targetOffsetZ =
                            (networkInputSnapshot.hmdPosZ - networkInputSnapshot.baselineHmdPosZ) * horizontalMoveScale;
                        const float deltaOffsetX = targetOffsetX - networkInputSnapshot.appliedHmdOffsetX;
                        const float deltaOffsetY = targetOffsetY - networkInputSnapshot.appliedHmdOffsetY;
                        const float deltaOffsetZ = targetOffsetZ - networkInputSnapshot.appliedHmdOffsetZ;
                        const float horizontalDeadzone = hmdPosDeadzone * horizontalMoveScale;
                        const float verticalDeadzone = hmdPosDeadzone * verticalMoveScale;

                        if (std::abs(deltaOffsetX) > horizontalDeadzone) {
                            camera_right(deltaOffsetX);
                        }
                        if (std::abs(deltaOffsetY) > verticalDeadzone) {
                            camera_up(deltaOffsetY);
                        }
                        if (std::abs(deltaOffsetZ) > horizontalDeadzone) {
                            camera_forward(deltaOffsetZ);
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(networkCameraInputMutex);
                        if (!networkCameraInputState.hasHmdOrigin) {
                            networkCameraInputState.baselineHmdPosX = networkInputSnapshot.hmdPosX;
                            networkCameraInputState.baselineHmdPosY = networkInputSnapshot.hmdPosY;
                            networkCameraInputState.baselineHmdPosZ = networkInputSnapshot.hmdPosZ;
                            networkCameraInputState.appliedHmdOffsetX = 0.0f;
                            networkCameraInputState.appliedHmdOffsetY = 0.0f;
                            networkCameraInputState.appliedHmdOffsetZ = 0.0f;
                            networkCameraInputState.hasHmdOrigin = true;
                        } else {
                            networkCameraInputState.appliedHmdOffsetX =
                                (networkCameraInputState.hmdPosX - networkCameraInputState.baselineHmdPosX)
                                * horizontalMoveScale;
                            networkCameraInputState.appliedHmdOffsetY =
                                (networkCameraInputState.hmdPosY - networkCameraInputState.baselineHmdPosY)
                                * verticalMoveScale;
                            networkCameraInputState.appliedHmdOffsetZ =
                                (networkCameraInputState.hmdPosZ - networkCameraInputState.baselineHmdPosZ)
                                * horizontalMoveScale;
                        }
                    }

                    clampBaseCameraPitch();

                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }).detach();
        }
    }

    void on_cam_network_input(
        float leftStickX,
        float leftStickY,
        float rightStickX,
        float rightStickY,
        float leftTrigger,
        float leftGrip,
        float rightTrigger,
        float rightGrip,
        float orientationX,
        float orientationY,
        float orientationZ,
        float orientationW,
        float hmdPosX,
        float hmdPosY,
        float hmdPosZ,
        int buttons,
        int flags,
        float ipdMeters,
        float hmdVerticalFovDegrees,
        float leftEyeAngleLeftRadians,
        float leftEyeAngleRightRadians,
        float leftEyeAngleUpRadians,
        float leftEyeAngleDownRadians,
        float rightEyeAngleLeftRadians,
        float rightEyeAngleRightRadians,
        float rightEyeAngleUpRadians,
        float rightEyeAngleDownRadians
    ) {
        constexpr float minFovAngle = -1.55f;
        constexpr float maxFovAngle = 1.55f;
        const float fallbackHalfVerticalRadians =
            std::clamp(hmdVerticalFovDegrees, 20.0f, 170.0f) * 0.00872664625f;
        auto clampFov = [&](float value, float fallbackValue) {
            if (!std::isfinite(value)) {
                return fallbackValue;
            }
            return std::clamp(value, minFovAngle, maxFovAngle);
        };

        std::lock_guard<std::mutex> lock(networkCameraInputMutex);
        const uint32_t inputFlags = static_cast<uint32_t>(flags);
        if ((inputFlags & networkInputFlagResetBaseline) != 0u) {
            networkCameraInputState.hasHmdOrigin = false;
            networkCameraInputState.appliedHmdOffsetX = 0.0f;
            networkCameraInputState.appliedHmdOffsetY = 0.0f;
            networkCameraInputState.appliedHmdOffsetZ = 0.0f;
            networkCameraInputState.hasHeadOrientationOrigin = false;
            networkCameraInputState.baselineOrientationX = 0.0f;
            networkCameraInputState.baselineOrientationY = 0.0f;
            networkCameraInputState.baselineOrientationZ = 0.0f;
            networkCameraInputState.baselineOrientationW = 1.0f;
            networkCameraInputState.smoothedYawOffsetDegrees = 0.0f;
            networkCameraInputState.smoothedPitchOffsetDegrees = 0.0f;
            networkCameraInputState.smoothedRollOffsetDegrees = 0.0f;
            networkCameraInputState.appliedYawOffsetDegrees = 0.0f;
            networkCameraInputState.appliedPitchOffsetDegrees = 0.0f;
            networkCameraInputState.appliedRollOffsetDegrees = 0.0f;
        }
        networkCameraInputState.leftStickX = std::clamp(leftStickX, -1.0f, 1.0f);
        networkCameraInputState.leftStickY = std::clamp(leftStickY, -1.0f, 1.0f);
        networkCameraInputState.rightStickX = std::clamp(rightStickX, -1.0f, 1.0f);
        networkCameraInputState.rightStickY = std::clamp(rightStickY, -1.0f, 1.0f);
        networkCameraInputState.leftTrigger = std::clamp(leftTrigger, 0.0f, 1.0f);
        networkCameraInputState.leftGrip = std::clamp(leftGrip, 0.0f, 1.0f);
        networkCameraInputState.rightTrigger = std::clamp(rightTrigger, 0.0f, 1.0f);
        networkCameraInputState.rightGrip = std::clamp(rightGrip, 0.0f, 1.0f);
        const auto normalizedOrientation = normalizeQuaternion({
            orientationX,
            orientationY,
            orientationZ,
            orientationW
        });
        networkCameraInputState.orientationX = normalizedOrientation.x;
        networkCameraInputState.orientationY = normalizedOrientation.y;
        networkCameraInputState.orientationZ = normalizedOrientation.z;
        networkCameraInputState.orientationW = normalizedOrientation.w;
        networkCameraInputState.hmdPosX = hmdPosX;
        networkCameraInputState.hmdPosY = hmdPosY;
        networkCameraInputState.hmdPosZ = hmdPosZ;
        networkCameraInputState.ipdMeters = std::clamp(ipdMeters, 0.0f, 0.12f);
        networkCameraInputState.hmdVerticalFovDegrees = std::clamp(hmdVerticalFovDegrees, 20.0f, 170.0f);
        networkCameraInputState.leftEyeAngleLeftRadians = clampFov(leftEyeAngleLeftRadians, -fallbackHalfVerticalRadians);
        networkCameraInputState.leftEyeAngleRightRadians = clampFov(leftEyeAngleRightRadians, fallbackHalfVerticalRadians);
        networkCameraInputState.leftEyeAngleUpRadians = clampFov(leftEyeAngleUpRadians, fallbackHalfVerticalRadians);
        networkCameraInputState.leftEyeAngleDownRadians = clampFov(leftEyeAngleDownRadians, -fallbackHalfVerticalRadians);
        networkCameraInputState.rightEyeAngleLeftRadians = clampFov(rightEyeAngleLeftRadians, -fallbackHalfVerticalRadians);
        networkCameraInputState.rightEyeAngleRightRadians = clampFov(rightEyeAngleRightRadians, fallbackHalfVerticalRadians);
        networkCameraInputState.rightEyeAngleUpRadians = clampFov(rightEyeAngleUpRadians, fallbackHalfVerticalRadians);
        networkCameraInputState.rightEyeAngleDownRadians = clampFov(rightEyeAngleDownRadians, -fallbackHalfVerticalRadians);
        if (networkCameraInputState.leftEyeAngleLeftRadians >= networkCameraInputState.leftEyeAngleRightRadians) {
            networkCameraInputState.leftEyeAngleLeftRadians = -fallbackHalfVerticalRadians;
            networkCameraInputState.leftEyeAngleRightRadians = fallbackHalfVerticalRadians;
        }
        if (networkCameraInputState.leftEyeAngleDownRadians >= networkCameraInputState.leftEyeAngleUpRadians) {
            networkCameraInputState.leftEyeAngleDownRadians = -fallbackHalfVerticalRadians;
            networkCameraInputState.leftEyeAngleUpRadians = fallbackHalfVerticalRadians;
        }
        if (networkCameraInputState.rightEyeAngleLeftRadians >= networkCameraInputState.rightEyeAngleRightRadians) {
            networkCameraInputState.rightEyeAngleLeftRadians = -fallbackHalfVerticalRadians;
            networkCameraInputState.rightEyeAngleRightRadians = fallbackHalfVerticalRadians;
        }
        if (networkCameraInputState.rightEyeAngleDownRadians >= networkCameraInputState.rightEyeAngleUpRadians) {
            networkCameraInputState.rightEyeAngleDownRadians = -fallbackHalfVerticalRadians;
            networkCameraInputState.rightEyeAngleUpRadians = fallbackHalfVerticalRadians;
        }
        networkCameraInputState.buttons = static_cast<uint32_t>(buttons);
        networkCameraInputState.flags = inputFlags;
        if (networkCameraInputState.leftStickX == 0.0f
            && networkCameraInputState.leftStickY == 0.0f
            && networkCameraInputState.rightStickX == 0.0f
            && networkCameraInputState.rightStickY == 0.0f
            && networkCameraInputState.leftTrigger == 0.0f
            && networkCameraInputState.leftGrip == 0.0f
            && networkCameraInputState.rightTrigger == 0.0f
            && networkCameraInputState.rightGrip == 0.0f
            && networkCameraInputState.hmdPosX == 0.0f
            && networkCameraInputState.hmdPosY == 0.0f
            && networkCameraInputState.hmdPosZ == 0.0f
            && networkCameraInputState.buttons == 0) {
            networkCameraInputState.hasHmdOrigin = false;
            networkCameraInputState.appliedHmdOffsetX = 0.0f;
            networkCameraInputState.appliedHmdOffsetY = 0.0f;
            networkCameraInputState.appliedHmdOffsetZ = 0.0f;
            networkCameraInputState.hasHeadOrientationOrigin = false;
            networkCameraInputState.baselineOrientationX = 0.0f;
            networkCameraInputState.baselineOrientationY = 0.0f;
            networkCameraInputState.baselineOrientationZ = 0.0f;
            networkCameraInputState.baselineOrientationW = 1.0f;
            networkCameraInputState.smoothedYawOffsetDegrees = 0.0f;
            networkCameraInputState.smoothedPitchOffsetDegrees = 0.0f;
            networkCameraInputState.smoothedRollOffsetDegrees = 0.0f;
            networkCameraInputState.appliedYawOffsetDegrees = 0.0f;
            networkCameraInputState.appliedPitchOffsetDegrees = 0.0f;
            networkCameraInputState.appliedRollOffsetDegrees = 0.0f;
        }
    }

    NetworkStereoConfig GetNetworkStereoConfig() {
        std::lock_guard<std::mutex> lock(networkCameraInputMutex);
        NetworkStereoConfig config{};
        config.ipdMeters = networkCameraInputState.ipdMeters;
        config.leftEyeAngleLeftRadians = networkCameraInputState.leftEyeAngleLeftRadians;
        config.leftEyeAngleRightRadians = networkCameraInputState.leftEyeAngleRightRadians;
        config.leftEyeAngleUpRadians = networkCameraInputState.leftEyeAngleUpRadians;
        config.leftEyeAngleDownRadians = networkCameraInputState.leftEyeAngleDownRadians;
        config.rightEyeAngleLeftRadians = networkCameraInputState.rightEyeAngleLeftRadians;
        config.rightEyeAngleRightRadians = networkCameraInputState.rightEyeAngleRightRadians;
        config.rightEyeAngleUpRadians = networkCameraInputState.rightEyeAngleUpRadians;
        config.rightEyeAngleDownRadians = networkCameraInputState.rightEyeAngleDownRadians;
        return config;
    }

    float GetFreeCameraRollDegrees() {
        std::lock_guard<std::mutex> lock(networkCameraInputMutex);
        return networkCameraInputState.appliedRollOffsetDegrees;
    }

    void ResetNetworkHeadTrackingState() {
        std::lock_guard<std::mutex> lock(networkCameraInputMutex);
        networkCameraInputState.hasHmdOrigin = false;
        networkCameraInputState.baselineHmdPosX = 0.0f;
        networkCameraInputState.baselineHmdPosY = 0.0f;
        networkCameraInputState.baselineHmdPosZ = 0.0f;
        networkCameraInputState.appliedHmdOffsetX = 0.0f;
        networkCameraInputState.appliedHmdOffsetY = 0.0f;
        networkCameraInputState.appliedHmdOffsetZ = 0.0f;
        networkCameraInputState.hasHeadOrientationOrigin = false;
        networkCameraInputState.baselineOrientationX = 0.0f;
        networkCameraInputState.baselineOrientationY = 0.0f;
        networkCameraInputState.baselineOrientationZ = 0.0f;
        networkCameraInputState.baselineOrientationW = 1.0f;
        networkCameraInputState.smoothedYawOffsetDegrees = 0.0f;
        networkCameraInputState.smoothedPitchOffsetDegrees = 0.0f;
        networkCameraInputState.smoothedRollOffsetDegrees = 0.0f;
        networkCameraInputState.appliedYawOffsetDegrees = 0.0f;
        networkCameraInputState.appliedPitchOffsetDegrees = 0.0f;
        networkCameraInputState.appliedRollOffsetDegrees = 0.0f;
    }

    void initCameraSettings() {
        reset_camera();
        cameraRawInputThread();
    }
}
