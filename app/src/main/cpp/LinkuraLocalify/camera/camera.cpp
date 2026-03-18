#include "baseCamera.hpp"
#include "camera.hpp"
#include "camera_internal.hpp"
#include <thread>
#include "../Misc.hpp"
#include "../BaseDefine.h"
#include "../../platformDefine.hpp"
#include "../Log.h"
#include "../config/Config.hpp"
#include <algorithm>
#include <cmath>

#ifdef GKMS_WINDOWS
    #include <corecrt_math_defines.h>
#endif // GKMS_WINDOWS



namespace L4Camera {
    constexpr float kMaxSafePitchDegrees = 85.0f;
	BaseCamera::Camera baseCamera{};
    BaseCamera::Camera originCamera{};
    CameraMode cameraMode = CameraMode::FREE;
    FirstPersonRoll firstPersonRoll = FirstPersonRoll::DISABLE_ROLL;
    FollowModeY followModeY = FollowModeY::SMOOTH_Y;

    CameraInfo currentCameraInfo;
    UnityResolve::UnityType::Color backgroundColor{0.0f, 0.0f, 0.0f, 1.0f};

    UnityResolve::UnityType::Vector3 firstPersonPosOffset{0, 0.064f, 0.000f};
    UnityResolve::UnityType::Vector3 followPosOffset{0, 0, 1.5};
    UnityResolve::UnityType::Vector2 followLookAtOffset{0, 0};
    float offsetMoveStep = 0.008;
    float l_sensitivity = 0.5f;
    float r_sensitivity = 0.5f;
    bool showToast = true;
    LinkuraLocal::Misc::CSEnum bodyPartsEnum("Head", 0xa);
    CharacterMeshFirstPersonManager<void*> followCharaSet;
    CharacterMeshRenderManager<void*> charaRenderSet;
    CameraMoveState cameraMoveState;

	// bool rMousePressFlg = false;

    void SetCameraMode(CameraMode mode) {
        cameraMode = mode;
    }

    CameraMode GetCameraMode() {
        return cameraMode;
    }

    void SetFirstPersonRoll(FirstPersonRoll mode) {
        firstPersonRoll = mode;
    }

    FirstPersonRoll GetFirstPersonRoll() {
        return firstPersonRoll;
    }

    void reset_camera() {
        firstPersonPosOffset = {0, 0.064f, 0.000f};  // f3: 0.008f
        followPosOffset = {0, 0, 1.5};
        followLookAtOffset = {0, 0};
        baseCamera.verticalAngle = 0.0f;
        baseCamera.horizontalAngle = 0.0f;
        baseCamera.setHoriLook(baseCamera.verticalAngle);
        originCamera.setCamera(&baseCamera);
        ResetNetworkHeadTrackingState();
	}

	void camera_forward(float multiplier = 1.0f) {  // 向前
        switch (cameraMode) {
            case CameraMode::FREE: {
                baseCamera.set_lon_move(0, LonMoveHState::LonMoveForward, multiplier);
            } break;
            case CameraMode::FIRST_PERSON: {
                firstPersonPosOffset.z += offsetMoveStep * multiplier;
            } break;
            case CameraMode::FOLLOW: {
                followPosOffset.z -= offsetMoveStep * multiplier;
            }
        }

	}
	void camera_back(float multiplier = 1.0f) {  // 后退
        switch (cameraMode) {
            case CameraMode::FREE: {
                baseCamera.set_lon_move(180, LonMoveHState::LonMoveBack, multiplier);
            } break;
            case CameraMode::FIRST_PERSON: {
                firstPersonPosOffset.z -= offsetMoveStep * multiplier;
            } break;
            case CameraMode::FOLLOW: {
                followPosOffset.z += offsetMoveStep * multiplier;
            }
        }
	}
	void camera_left(float multiplier = 1.0f) {  // 向左
        switch (cameraMode) {
            case CameraMode::FREE: {
                baseCamera.set_lon_move(90, LonMoveLeftAndRight, multiplier);
            } break;
            case CameraMode::FOLLOW: {
                // followPosOffset.x += 0.8;
                followLookAtOffset.x += offsetMoveStep * multiplier;
            }
            default:
                break;
        }

	}
	void camera_right(float multiplier = 1.0f) {  // 向右
        switch (cameraMode) {
            case CameraMode::FREE: {
                baseCamera.set_lon_move(-90, LonMoveLeftAndRight, multiplier);
            } break;
            case CameraMode::FOLLOW: {
                // followPosOffset.x -= 0.8;
                followLookAtOffset.x -= offsetMoveStep * multiplier;
            }
            default:
                break;
        }
	}

	void camera_down(float multiplier = 1.0f) {  // 向下
        switch (cameraMode) {
            case CameraMode::FREE: {
                float preStep = BaseCamera::moveStep / BaseCamera::smoothLevel * multiplier;

                for (int i = 0; i < BaseCamera::smoothLevel; i++) {
                    baseCamera.pos.y -= preStep;
                    baseCamera.lookAt.y -= preStep;
                    std::this_thread::sleep_for(std::chrono::milliseconds(BaseCamera::sleepTime));
                }
            } break;
            case CameraMode::FIRST_PERSON: {
                firstPersonPosOffset.y -= offsetMoveStep * multiplier;
            } break;
            case CameraMode::FOLLOW: {
                // followPosOffset.y -= offsetMoveStep;
                followLookAtOffset.y -= offsetMoveStep * multiplier;
            }
        }
	}

	void camera_up(float multiplier = 1.0f) {  // 向上
        switch (cameraMode) {
            case CameraMode::FREE: {
                float preStep = BaseCamera::moveStep / BaseCamera::smoothLevel * multiplier;

                for (int i = 0; i < BaseCamera::smoothLevel; i++) {
                    baseCamera.pos.y += preStep;
                    baseCamera.lookAt.y += preStep;
                    std::this_thread::sleep_for(std::chrono::milliseconds(BaseCamera::sleepTime));
                }
            } break;
            case CameraMode::FIRST_PERSON: {
                firstPersonPosOffset.y += offsetMoveStep * multiplier;
            } break;
            case CameraMode::FOLLOW: {
                // followPosOffset.y += offsetMoveStep;
                followLookAtOffset.y += offsetMoveStep * multiplier;
            }
        }
	}
	void cameraLookat_up(float mAngel, bool mouse) {
		baseCamera.horizontalAngle += mAngel * LinkuraLocal::Config::cameraRotationSensitivity;
		if (baseCamera.horizontalAngle >= kMaxSafePitchDegrees) baseCamera.horizontalAngle = kMaxSafePitchDegrees;
		baseCamera.updateVertLook();
	}
	void cameraLookat_down(float mAngel, bool mouse) {
		baseCamera.horizontalAngle -= mAngel * LinkuraLocal::Config::cameraRotationSensitivity;
		if (baseCamera.horizontalAngle <= -kMaxSafePitchDegrees) baseCamera.horizontalAngle = -kMaxSafePitchDegrees;
		baseCamera.updateVertLook();
	}
	void cameraLookat_left(float mAngel) {
		baseCamera.verticalAngle += mAngel * LinkuraLocal::Config::cameraRotationSensitivity;
		if (baseCamera.verticalAngle >= 360) baseCamera.verticalAngle = -360;
		baseCamera.setHoriLook(baseCamera.verticalAngle);
	}
	void cameraLookat_right(float mAngel) {
		baseCamera.verticalAngle -= mAngel * LinkuraLocal::Config::cameraRotationSensitivity;
		if (baseCamera.verticalAngle <= -360) baseCamera.verticalAngle = 360;
		baseCamera.setHoriLook(baseCamera.verticalAngle);
	}
	void changeCameraFOV(float value) {
		baseCamera.fov += value * LinkuraLocal::Config::cameraFovSensitivity;
	}

    void SwitchCameraMode() {
        switch (cameraMode) {
            case CameraMode::FREE: {
                cameraMode = CameraMode::FOLLOW;
                LinkuraLocal::Log::Info("CameraMode: FOLLOW");
            } break;
            case CameraMode::FOLLOW: {
                cameraMode = CameraMode::FIRST_PERSON;
                LinkuraLocal::Log::Info("CameraMode: FIRST_PERSON");
            } break;
            case CameraMode::FIRST_PERSON: {
                SyncBaseCameraFromCurrentMode();
                cameraMode = CameraMode::FREE;
                LinkuraLocal::Log::Info("CameraMode: FREE");
            } break;
        }
        L4Camera::followCharaSet.onCameraModeChange(cameraMode);
    }

    void SwitchCameraSubMode() {
        switch (cameraMode) {
            case CameraMode::FIRST_PERSON: {
                if (firstPersonRoll == FirstPersonRoll::ENABLE_ROLL) {
                    firstPersonRoll = FirstPersonRoll::DISABLE_ROLL;
                    LinkuraLocal::Log::Info("FirstPersonRoll: DISABLE_ROLL");
                }
                else {
                    firstPersonRoll = FirstPersonRoll::ENABLE_ROLL;
                    LinkuraLocal::Log::Info("FirstPersonRoll: ENABLE_ROLL");
                }
            } break;

            case CameraMode::FOLLOW: {
                if (followModeY == FollowModeY::APPLY_Y) {
                    followModeY = FollowModeY::SMOOTH_Y;
                    LinkuraLocal::Log::Info("FollowModeY: SMOOTH_Y");
                }
                else {
                    followModeY = FollowModeY::APPLY_Y;
                    LinkuraLocal::Log::Info("FollowModeY: APPLY_Y");
                }
            } break;

            default: break;
        }
    }

    bool SelectPreviousCharacterTarget() {
        if (cameraMode == CameraMode::FREE) {
            return false;
        }
        if (LinkuraLocal::HookCamera::CanHandleSchoolIdleTargetSwitchInput()) {
            return LinkuraLocal::HookCamera::RequestPreviousSchoolIdleTargetSwitch();
        }
        if (followCharaSet.size() == 0) {
            return false;
        }
        followCharaSet.prev();
        return true;
    }

    bool SelectNextCharacterTarget() {
        if (cameraMode == CameraMode::FREE) {
            return false;
        }
        if (LinkuraLocal::HookCamera::CanHandleSchoolIdleTargetSwitchInput()) {
            return LinkuraLocal::HookCamera::RequestNextSchoolIdleTargetSwitch();
        }
        if (followCharaSet.size() == 0) {
            return false;
        }
        followCharaSet.next();
        return true;
    }

    void OnLeftDown() {
        SelectPreviousCharacterTarget();
    }

    void OnRightDown() {
        SelectNextCharacterTarget();
    }

    void OnUpDown() {
        if (cameraMode == CameraMode::FOLLOW) {
            const auto currPart = bodyPartsEnum.Last();
            LinkuraLocal::Log::InfoFmt("Look at: %s (0x%x)", currPart.first.c_str(), currPart.second);
        }
    }

    void OnDownDown() {
        if (cameraMode == CameraMode::FOLLOW) {
            const auto currPart = bodyPartsEnum.Next();
            LinkuraLocal::Log::InfoFmt("Look at: %s (0x%x)", currPart.first.c_str(), currPart.second);
        }
    }

    void ChangeLiveFollowCameraOffsetY(const float value) {
        if (cameraMode == CameraMode::FOLLOW) {
            followPosOffset.y += value;
        }
    }

    void ChangeLiveFollowCameraOffsetX(const float value) {
        if (cameraMode == CameraMode::FOLLOW) {
            followPosOffset.x += value;
        }
    }

    void ShowToast(const char *text) {
        if (showToast) {
            LinkuraLocal::Log::ShowToast(text);
        }
    }

    void JLThumbRight(float value) {
        camera_right(value * l_sensitivity * LinkuraLocal::Config::cameraMovementSensitivity * baseCamera.fov / 60);
    }

    void JLThumbDown(float value) {
        camera_back(value * l_sensitivity * LinkuraLocal::Config::cameraMovementSensitivity * baseCamera.fov / 60);
    }

    void JRThumbRight(float value) {
        cameraLookat_right(value * r_sensitivity * LinkuraLocal::Config::cameraRotationSensitivity * baseCamera.fov / 60);
        ChangeLiveFollowCameraOffsetX(-1 * value * r_sensitivity * LinkuraLocal::Config::cameraRotationSensitivity * baseCamera.fov / 60);
    }

    void JRThumbDown(float value) {
        cameraLookat_down(value * r_sensitivity * LinkuraLocal::Config::cameraRotationSensitivity * baseCamera.fov / 60);
        ChangeLiveFollowCameraOffsetY(-0.1 * value * r_sensitivity * LinkuraLocal::Config::cameraRotationSensitivity * baseCamera.fov / 60);
    }

    void JDadUp(){
        reset_camera();
        ShowToast("Reset Camera");
    }

    void JDadDown(){
        ShowToast("Notification off, click again to turn it on.");
        showToast = !showToast;
    }

    void JDadLeft(){
        l_sensitivity = 1.0f;
        ShowToast("Reset Movement Sensitivity");
    }

    void JDadRight(){
        r_sensitivity = 1.0f;
        ShowToast("Reset Camera Sensitivity");
    }

    void JAKeyDown() {
        if (cameraMode == CameraMode::FOLLOW) {
            const auto currPart = bodyPartsEnum.Next();
            if (showToast) {
                LinkuraLocal::Log::ShowToastFmt("Look at: %s (0x%x)", currPart.first.c_str(),
                                                currPart.second);
            }
        } else {
            r_sensitivity *= 0.8f;
        }
    }

    void JBKeyDown() {
        if (cameraMode == CameraMode::FOLLOW) {
            const auto currPart = bodyPartsEnum.Last();
            if (showToast) {
                LinkuraLocal::Log::ShowToastFmt("Look at: %s (0x%x)", currPart.first.c_str(),
                                                currPart.second);
            }
        } else {
            r_sensitivity *= 1.2f;
        }
    }

    void JXKeyDown() {
        if (cameraMode == CameraMode::FOLLOW) {
            OnLeftDown();
        } else {
            l_sensitivity *= 0.8f;
        }
    }

    void JYKeyDown() {
        if (cameraMode == CameraMode::FOLLOW) {
            OnRightDown();
        } else {
            l_sensitivity *= 1.2f;
        }
    }

    void JSelectKeyDown() {
        switch (cameraMode) {
            case CameraMode::FREE: {
                cameraMode = CameraMode::FOLLOW;
                ShowToast("Follow Mode");
            } break;
            case CameraMode::FOLLOW: {
                cameraMode = CameraMode::FIRST_PERSON;
                ShowToast("First-person Mode");
            } break;
            case CameraMode::FIRST_PERSON: {
                SyncBaseCameraFromCurrentMode();
                cameraMode = CameraMode::FREE;
                ShowToast("Free Mode");
            } break;
        }
    }

    void JStartKeyDown() {
        switch (cameraMode) {
            case CameraMode::FIRST_PERSON: {
                if (firstPersonRoll == FirstPersonRoll::ENABLE_ROLL) {
                    firstPersonRoll = FirstPersonRoll::DISABLE_ROLL;
                    ShowToast("Camera Horizontal Fixed");
                }
                else {
                    firstPersonRoll = FirstPersonRoll::ENABLE_ROLL;
                    ShowToast("Camera Horizontal Rollable");
                }
            } break;

            case CameraMode::FOLLOW: {
                if (followModeY == FollowModeY::APPLY_Y) {
                    followModeY = FollowModeY::SMOOTH_Y;
                    ShowToast("Smooth Lift");
                }
                else {
                    followModeY = FollowModeY::APPLY_Y;
                    ShowToast("Instant Lift");
                }
            } break;

            default: break;
        }
    }

    UnityResolve::UnityType::Vector3 CalcPositionFromLookAt(const UnityResolve::UnityType::Vector3& target,
                                                            const UnityResolve::UnityType::Vector3& offset) {
        // offset: z 远近, y 高低, x角度
        const float angleX = offset.x;
        const float distanceZ = offset.z;
        const float angleRad = angleX * (M_PI / 180.0f);
        const float newX = target.x + distanceZ * std::sin(angleRad);
        const float newZ = target.z + distanceZ * std::cos(angleRad);
        const float newY = target.y + offset.y;
        return UnityResolve::UnityType::Vector3(newX, newY, newZ);
    }

    float CheckNewY(const UnityResolve::UnityType::Vector3& targetPos, const bool recordY,
                    LinkuraLocal::Misc::FixedSizeQueue<float>& recordsY) {
        const auto currentY = targetPos.y;
        static auto lastRetY = currentY;

        if (followModeY == FollowModeY::APPLY_Y) {
            lastRetY = currentY;
            return currentY;
        }

        const auto currentAvg = recordsY.Average();
        // LinkuraLocal::Log::DebugFmt("currentY: %f, currentAvg: %f, diff: %f", currentY, currentAvg, abs(currentY - currentAvg));

        if (recordY) {
            recordsY.Push(currentY);
        }

        if (abs(currentY - currentAvg) < 0.02) {
            return lastRetY;
        }

        const auto retAvg = recordsY.Average();
        lastRetY = lastRetY + (retAvg - lastRetY) / 8;
        return lastRetY;
    }

    UnityResolve::UnityType::Vector3 CalcFollowModeLookAt(const UnityResolve::UnityType::Vector3& targetPos,
                                                          const UnityResolve::UnityType::Vector3& posOffset,
                                                          const bool recordY) {
        static LinkuraLocal::Misc::FixedSizeQueue<float> recordsY(60);

        const float angleX = posOffset.x;
        const float angleRad = (angleX + (followPosOffset.z >= 0 ? 90.0f : -90.0f)) * (M_PI / 180.0f);

        UnityResolve::UnityType::Vector3 newTargetPos = targetPos;
        newTargetPos.y = CheckNewY(targetPos, recordY, recordsY);

        const float offsetX = followLookAtOffset.x * sin(angleRad);
        const float offsetZ = followLookAtOffset.x * cos(angleRad);

        newTargetPos.x += offsetX;
        newTargetPos.z += offsetZ;
        newTargetPos.y += followLookAtOffset.y;

        return newTargetPos;
    }

    UnityResolve::UnityType::Vector3 CalcFirstPersonPosition(const UnityResolve::UnityType::Vector3& position,
                                                             const UnityResolve::UnityType::Vector3& forward,
                                                             const UnityResolve::UnityType::Vector3& offset) {
        using Vector3 = UnityResolve::UnityType::Vector3;

        // 计算角色的右方向
        Vector3 up(0, 1, 0); // Y轴方向
        Vector3 right = forward.cross(up).Normalize();
        Vector3 fwd = forward;
        Vector3 pos = position;

        // 计算角色的左方向
        Vector3 left = right * -1.0f;

        // 计算最终位置
        Vector3 backwardOffset = fwd * -offset.z;
        Vector3 leftOffset = left * offset.x;

        Vector3 finalPosition = pos + backwardOffset + leftOffset;
        finalPosition.y += offset.y;

        return finalPosition;

    }

    void clampBaseCameraPitch() {
        const auto clampedPitch = std::clamp(
            baseCamera.horizontalAngle,
            -kMaxSafePitchDegrees,
            kMaxSafePitchDegrees
        );
        if (std::abs(clampedPitch - baseCamera.horizontalAngle) > 0.0001f) {
            baseCamera.horizontalAngle = clampedPitch;
            baseCamera.updateVertLook();
        }
    }

    void CameraMoveState::resetAll() {
        unsigned char* bytes = reinterpret_cast<unsigned char*>(this);
        for (size_t offset = 0; offset < sizeof(*this); ) {
            if (offset + sizeof(bool) <= sizeof(*this)
                && reinterpret_cast<bool*>(bytes + offset) == reinterpret_cast<bool*>(this) + offset / sizeof(bool)) {
                *reinterpret_cast<bool*>(bytes + offset) = false;
                offset += sizeof(bool);
            } else if (offset + sizeof(float) <= sizeof(*this)
                && reinterpret_cast<float*>(bytes + offset) == reinterpret_cast<float*>(this) + offset / sizeof(float)) {
                *reinterpret_cast<float*>(bytes + offset) = 0.0f;
                offset += sizeof(float);
            } else {
                offset += 1;
            }
        }
    }

    void clearRenderSet() {
        followCharaSet.clear();
        charaRenderSet.clear();
    }

    void UpdateCameraInfo(const UnityResolve::UnityType::Vector3& pos, 
                         const UnityResolve::UnityType::Quaternion& rot,
                         float fieldOfView) {
        currentCameraInfo.position = pos;
        currentCameraInfo.rotation = rot;
        currentCameraInfo.fov = fieldOfView;
        currentCameraInfo.isValid = true;
    }

    CameraInfo GetCurrentCameraInfo() {
        return currentCameraInfo;
    }

}
