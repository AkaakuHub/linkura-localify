#include "baseCamera.hpp"
#include "camera.hpp"
#include <thread>
#include "../Misc.hpp"
#include "../BaseDefine.h"
#include "../../platformDefine.hpp"
#include "../Log.h"
#include "../config/Config.hpp"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <mutex>

#ifdef GKMS_WINDOWS
    #include <corecrt_math_defines.h>
#endif // GKMS_WINDOWS



namespace L4Camera {
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
		baseCamera.reset();
        originCamera.reset();
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
	void cameraLookat_up(float mAngel, bool mouse = false) {
		baseCamera.horizontalAngle += mAngel * LinkuraLocal::Config::cameraRotationSensitivity;
		if (baseCamera.horizontalAngle >= 90) baseCamera.horizontalAngle = 89.99;
		baseCamera.updateVertLook();
	}
	void cameraLookat_down(float mAngel, bool mouse = false) {
		baseCamera.horizontalAngle -= mAngel * LinkuraLocal::Config::cameraRotationSensitivity;
		if (baseCamera.horizontalAngle <= -90) baseCamera.horizontalAngle = -89.99;
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

    void OnLeftDown() {
        if (cameraMode == CameraMode::FREE) return;
        L4Camera::followCharaSet.prev();
    }

    void OnRightDown() {
        if (cameraMode == CameraMode::FREE) return;
        L4Camera::followCharaSet.next();
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

	struct CameraMoveState {
		bool w = false;
		bool s = false;
		bool a = false;
		bool d = false;
		bool ctrl = false;
		bool space = false;
		bool up = false;
		bool down = false;
		bool left = false;
		bool right = false;
		bool q = false;
		bool e = false;
        bool r = false;
		bool i = false;
		bool k = false;
		bool j = false;
		bool l = false;
        float thumb_l_right = 0.0f;
        float thumb_l_down = 0.0f;
        bool thumb_l_button = false;
        float thumb_r_right = 0.0f;
        float thumb_r_down = 0.0f;
        bool thumb_r_button = false;
        bool dpad_up = false;
        bool dpad_down = false;
        bool dpad_left = false;
        bool dpad_right = false;
        bool a_button = false;
        bool b_button = false;
        bool x_button = false;
        bool y_button = false;
        bool lb_button = false;
        float lt_button = 0.0f;
        bool rb_button = false;
        float rt_button = 0.0f;
        bool select_button = false;
        bool start_button = false;
        bool share_button = false;
        bool xbox_button = false;
		bool threadRunning = false;

		void resetAll() {
            // 获取当前对象的指针并转换为 unsigned char* 类型
            unsigned char* p = reinterpret_cast<unsigned char*>(this);

            // 遍历对象的每个字节
            for (size_t offset = 0; offset < sizeof(*this); ) {
                if (offset + sizeof(bool) <= sizeof(*this) && reinterpret_cast<bool*>(p + offset) == reinterpret_cast<bool*>(this) + offset / sizeof(bool)) {
                    // 如果当前偏移量适用于 bool 类型，则将其设置为 false
                    *reinterpret_cast<bool*>(p + offset) = false;
                    offset += sizeof(bool);
                } else if (offset + sizeof(float) <= sizeof(*this) && reinterpret_cast<float*>(p + offset) == reinterpret_cast<float*>(this) + offset / sizeof(float)) {
                    // 如果当前偏移量适用于 float 类型，则将其设置为 0.0
                    *reinterpret_cast<float*>(p + offset) = 0.0f;
                    offset += sizeof(float);
                } else {
                    // 处理未定义的情况（例如混合类型数组或其他类型成员）
                    // 可以根据实际情况调整逻辑或添加更多类型检查
                    offset += 1; // 跳过一个字节
                }
            }
		}
	} cameraMoveState;

    struct NetworkCameraInputState {
        float leftStickX = 0.0f;
        float leftStickY = 0.0f;
        float rightStickX = 0.0f;
        float rightStickY = 0.0f;
        float leftTrigger = 0.0f;
        float leftGrip = 0.0f;
        float rightTrigger = 0.0f;
        float rightGrip = 0.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;
        float hmdPosX = 0.0f;
        float hmdPosY = 0.0f;
        float hmdPosZ = 0.0f;
        float prevHmdPosX = 0.0f;
        float prevHmdPosY = 0.0f;
        float prevHmdPosZ = 0.0f;
        float prevYaw = 0.0f;
        float prevPitch = 0.0f;
        float filteredYawDelta = 0.0f;
        float filteredPitchDelta = 0.0f;
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
        bool hasPrevHmdPose = false;
        bool hasPrevHeadAngle = false;
        uint32_t buttons = 0;
        uint32_t prevButtons = 0;
        uint32_t flags = 0;
    } networkCameraInputState;
    std::mutex networkCameraInputMutex;

    float normalizeRadianDelta(float value) {
        constexpr float twoPi = 2.0f * static_cast<float>(M_PI);
        while (value > static_cast<float>(M_PI)) {
            value -= twoPi;
        }
        while (value < -static_cast<float>(M_PI)) {
            value += twoPi;
        }
        return value;
    }

    float clampAbs(float value, float maxAbs) {
        return std::clamp(value, -maxAbs, maxAbs);
    }


	void cameraRawInputThread() {
		using namespace BaseCamera;

		std::thread([]() {
			if (cameraMoveState.threadRunning) return;
			cameraMoveState.threadRunning = true;
            int moveSensitivityStickLatch = 0;
            int rotationSensitivityStickLatch = 0;
			while (true) {
                NetworkCameraInputState networkInputSnapshot;
                {
                    std::lock_guard<std::mutex> lock(networkCameraInputMutex);
                    networkInputSnapshot = networkCameraInputState;
                }
                constexpr uint32_t buttonB = (1u << 1);
                constexpr uint32_t buttonY = (1u << 3);
                const bool isBPressed = (networkInputSnapshot.buttons & buttonB) != 0;
                const bool isYPressed = (networkInputSnapshot.buttons & buttonY) != 0;
                if (networkInputSnapshot.hmdVerticalFovDegrees >= 20.0f &&
                    networkInputSnapshot.hmdVerticalFovDegrees <= 170.0f) {
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
				if (cameraMoveState.j) ChangeLiveFollowCameraOffsetX(0.8);
				if (cameraMoveState.l) ChangeLiveFollowCameraOffsetX(-0.8);
                // 手柄操作响应
                // 左摇杆
                if (std::abs(cameraMoveState.thumb_l_right) > 0.1f)
                    JLThumbRight(cameraMoveState.thumb_l_right);
                if (std::abs(cameraMoveState.thumb_l_down) > 0.1f)
                    JLThumbDown(cameraMoveState.thumb_l_down);
                // 右摇杆
                if (std::abs(cameraMoveState.thumb_r_right) > 0.1f)
                    JRThumbRight(cameraMoveState.thumb_r_right);
                if (std::abs(cameraMoveState.thumb_r_down) > 0.1f)
                    JRThumbDown(cameraMoveState.thumb_r_down);
                // 左扳机
                if (std::abs(cameraMoveState.lt_button) > 0.1f)
                    camera_down(cameraMoveState.lt_button * l_sensitivity * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60);
                // 右扳机
                if (std::abs(cameraMoveState.rt_button) > 0.1f)
                    camera_up(cameraMoveState.rt_button * l_sensitivity * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60);
                // 左肩键
                if (cameraMoveState.lb_button) changeCameraFOV(0.5f * r_sensitivity * LinkuraLocal::Config::cameraFovSensitivity);
                // 右肩键
                if (cameraMoveState.rb_button) changeCameraFOV(-0.5f * r_sensitivity * LinkuraLocal::Config::cameraFovSensitivity);
                // 十字键
                if (cameraMoveState.dpad_up) JDadUp();
//                if (cameraMoveState.dpad_down) JDadDown();
                if (cameraMoveState.dpad_left) JDadLeft();
                if (cameraMoveState.dpad_right) JDadRight();

                if (std::abs(networkInputSnapshot.leftStickX) > 0.01f) {
                    camera_right(networkInputSnapshot.leftStickX * l_sensitivity *
                                 LinkuraLocal::Config::cameraMovementSensitivity * baseCamera.fov / 60);
                }
                if (!isBPressed && std::abs(networkInputSnapshot.leftStickY) > 0.01f) {
                    camera_forward(networkInputSnapshot.leftStickY * l_sensitivity *
                                   LinkuraLocal::Config::cameraMovementSensitivity * baseCamera.fov / 60);
                }
                if (std::abs(networkInputSnapshot.rightStickX) > 0.01f) {
                    JRThumbRight(networkInputSnapshot.rightStickX);
                }
                if (!isYPressed && std::abs(networkInputSnapshot.rightStickY) > 0.01f) {
                    JRThumbDown(-networkInputSnapshot.rightStickY);
                }

                const float cameraYawInputRad = networkInputSnapshot.pitch;
                const float cameraPitchInputRad = networkInputSnapshot.roll;
                float yawDeltaRad = 0.0f;
                float pitchDeltaRad = 0.0f;
                if (networkInputSnapshot.hasPrevHeadAngle) {
                    yawDeltaRad = normalizeRadianDelta(cameraYawInputRad - networkInputSnapshot.prevYaw);
                    pitchDeltaRad = normalizeRadianDelta(cameraPitchInputRad - networkInputSnapshot.prevPitch);
                }

                yawDeltaRad = clampAbs(yawDeltaRad, 0.2f);
                pitchDeltaRad = clampAbs(pitchDeltaRad, 0.2f);
                constexpr float yprSmoothing = 0.2f;
                constexpr float yprDeadzoneRad = 0.002f;
                yawDeltaRad = networkInputSnapshot.filteredYawDelta + (yawDeltaRad - networkInputSnapshot.filteredYawDelta) * yprSmoothing;
                pitchDeltaRad = networkInputSnapshot.filteredPitchDelta + (pitchDeltaRad - networkInputSnapshot.filteredPitchDelta) * yprSmoothing;
                if (std::abs(yawDeltaRad) < yprDeadzoneRad) yawDeltaRad = 0.0f;
                if (std::abs(pitchDeltaRad) < yprDeadzoneRad) pitchDeltaRad = 0.0f;

                {
                    std::lock_guard<std::mutex> lock(networkCameraInputMutex);
                    networkCameraInputState.prevYaw = cameraYawInputRad;
                    networkCameraInputState.prevPitch = cameraPitchInputRad;
                    networkCameraInputState.filteredYawDelta = yawDeltaRad;
                    networkCameraInputState.filteredPitchDelta = pitchDeltaRad;
                    networkCameraInputState.hasPrevHeadAngle = true;
                }

                constexpr float hmdRotationGain = 3.0f;
                constexpr float radToStickScale = 35.0f;
                const float hmdYawStick = std::clamp(-yawDeltaRad * radToStickScale * hmdRotationGain, -1.0f, 1.0f);
                const float hmdPitchStick = std::clamp(-pitchDeltaRad * radToStickScale * hmdRotationGain, -1.0f, 1.0f);
                if (std::abs(hmdYawStick) > 0.001f) {
                    JRThumbRight(hmdYawStick);
                }
                if (std::abs(hmdPitchStick) > 0.001f) {
                    JRThumbDown(hmdPitchStick);
                }

                const auto currentButtons = networkInputSnapshot.buttons;
                const auto previousButtons = networkInputSnapshot.prevButtons;
                const auto risingButtons = (~previousButtons) & currentButtons;
                {
                    std::lock_guard<std::mutex> lock(networkCameraInputMutex);
                    networkCameraInputState.prevButtons = currentButtons;
                }

                constexpr float sensitivityAdjustScale = 0.5f;
                constexpr float sensitivityMin = 0.05f;
                constexpr float sensitivityMax = 10.0f;
                constexpr float sensitivityStickTriggerThreshold = 0.6f;
                constexpr float sensitivityStickReleaseThreshold = 0.25f;
                constexpr uint32_t buttonA = (1u << 0);
                constexpr uint32_t buttonX = (1u << 2);
                const bool isAPressed = (currentButtons & buttonA) != 0;
                const bool isXPressed = (currentButtons & buttonX) != 0;
                const bool canHandleFesLiveViewSwitch = LinkuraLocal::HookCamera::CanHandleFesLiveViewSwitchInput();
                const bool canHandleSchoolIdleTargetSwitch = LinkuraLocal::HookCamera::CanHandleSchoolIdleTargetSwitchInput();

                // Trigger FesLive view changes on button press without breaking the existing hold modifiers.
                if ((risingButtons & buttonY) && canHandleFesLiveViewSwitch) {
                    LinkuraLocal::HookCamera::RequestNextFesLiveViewSwitch();
                }
                if ((risingButtons & buttonB) && canHandleSchoolIdleTargetSwitch) {
                    LinkuraLocal::HookCamera::RequestNextSchoolIdleTargetSwitch();
                }

                if (!isBPressed) {
                    moveSensitivityStickLatch = 0;
                } else {
                    if (std::abs(networkInputSnapshot.leftStickY) <= sensitivityStickReleaseThreshold) {
                        moveSensitivityStickLatch = 0;
                    }
                    int currentStickDirection = 0;
                    if (networkInputSnapshot.leftStickY >= sensitivityStickTriggerThreshold) {
                        currentStickDirection = 1;
                    } else if (networkInputSnapshot.leftStickY <= -sensitivityStickTriggerThreshold) {
                        currentStickDirection = -1;
                    }
                    if (currentStickDirection != 0 && moveSensitivityStickLatch != currentStickDirection) {
                        LinkuraLocal::Config::cameraMovementSensitivity = std::clamp(
                            LinkuraLocal::Config::cameraMovementSensitivity + (currentStickDirection * sensitivityAdjustScale),
                            sensitivityMin,
                            sensitivityMax
                        );
                        moveSensitivityStickLatch = currentStickDirection;
                        if (showToast) {
                            LinkuraLocal::Log::ShowToastFmt("Movement Sensitivity: %.2f", LinkuraLocal::Config::cameraMovementSensitivity);
                        }
                    }
                }

                if (!isYPressed) {
                    rotationSensitivityStickLatch = 0;
                } else {
                    if (std::abs(networkInputSnapshot.rightStickY) <= sensitivityStickReleaseThreshold) {
                        rotationSensitivityStickLatch = 0;
                    }
                    int currentStickDirection = 0;
                    if (networkInputSnapshot.rightStickY >= sensitivityStickTriggerThreshold) {
                        currentStickDirection = 1;
                    } else if (networkInputSnapshot.rightStickY <= -sensitivityStickTriggerThreshold) {
                        currentStickDirection = -1;
                    }
                    if (currentStickDirection != 0 && rotationSensitivityStickLatch != currentStickDirection) {
                        LinkuraLocal::Config::cameraRotationSensitivity = std::clamp(
                            LinkuraLocal::Config::cameraRotationSensitivity + (currentStickDirection * sensitivityAdjustScale),
                            sensitivityMin,
                            sensitivityMax
                        );
                        rotationSensitivityStickLatch = currentStickDirection;
                        if (showToast) {
                            LinkuraLocal::Log::ShowToastFmt("Rotation Sensitivity: %.2f", LinkuraLocal::Config::cameraRotationSensitivity);
                        }
                    }
                }

                if (isYPressed && (risingButtons & buttonA)) {
                    LinkuraLocal::Config::cameraVerticalSensitivity = std::clamp(
                        LinkuraLocal::Config::cameraVerticalSensitivity + sensitivityAdjustScale,
                        sensitivityMin,
                        sensitivityMax
                    );
                    if (showToast) {
                        LinkuraLocal::Log::ShowToastFmt("Vertical Sensitivity: %.2f", LinkuraLocal::Config::cameraVerticalSensitivity);
                    }
                }
                if (isBPressed && (risingButtons & buttonX)) {
                    LinkuraLocal::Config::cameraVerticalSensitivity = std::clamp(
                        LinkuraLocal::Config::cameraVerticalSensitivity - sensitivityAdjustScale,
                        sensitivityMin,
                        sensitivityMax
                    );
                    if (showToast) {
                        LinkuraLocal::Log::ShowToastFmt("Vertical Sensitivity: %.2f", LinkuraLocal::Config::cameraVerticalSensitivity);
                    }
                }

                if (isAPressed && !isYPressed) {
                    camera_up(l_sensitivity * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60);
                }
                if (isXPressed && !isBPressed) {
                    camera_down(l_sensitivity * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60);
                }

                if (risingButtons & (1u << 4)) {
                    firstPersonPosOffset = {0, 0.064f, 0.000f};
                    followPosOffset = {0, 0, 1.5};
                    followLookAtOffset = {0, 0};
                }
                if (risingButtons & (1u << 5)) {
                    reset_camera();
                }

                if (networkInputSnapshot.hasPrevHmdPose) {
                    const float deltaHmdX = networkInputSnapshot.hmdPosX - networkInputSnapshot.prevHmdPosX;
                    const float deltaHmdY = networkInputSnapshot.hmdPosY - networkInputSnapshot.prevHmdPosY;
                    const float deltaHmdZ = networkInputSnapshot.hmdPosZ - networkInputSnapshot.prevHmdPosZ;
                    constexpr float hmdPosGain = 20.0f;
                    constexpr float hmdPosDeadzone = 0.00005f;

                    if (std::abs(deltaHmdX) > hmdPosDeadzone) {
                        camera_right(deltaHmdX * hmdPosGain * l_sensitivity * LinkuraLocal::Config::cameraMovementSensitivity * baseCamera.fov / 60);
                    }
                    if (std::abs(deltaHmdY) > hmdPosDeadzone) {
                        camera_up(deltaHmdY * hmdPosGain * l_sensitivity * LinkuraLocal::Config::cameraVerticalSensitivity * baseCamera.fov / 60);
                    }
                    if (std::abs(deltaHmdZ) > hmdPosDeadzone) {
                        camera_forward(deltaHmdZ * hmdPosGain * l_sensitivity * LinkuraLocal::Config::cameraMovementSensitivity * baseCamera.fov / 60);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(networkCameraInputMutex);
                    networkCameraInputState.prevHmdPosX = networkInputSnapshot.hmdPosX;
                    networkCameraInputState.prevHmdPosY = networkInputSnapshot.hmdPosY;
                    networkCameraInputState.prevHmdPosZ = networkInputSnapshot.hmdPosZ;
                    networkCameraInputState.hasPrevHmdPose = true;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			}).detach();
	}

	void on_cam_rawinput_keyboard(int message, int key) {
		if (message == WM_KEYDOWN || message == WM_KEYUP) {
			switch (key) {
			case KEY_W:
				cameraMoveState.w = message == WM_KEYDOWN; break;
			case KEY_S:
				cameraMoveState.s = message == WM_KEYDOWN; break;
			case KEY_A:
				cameraMoveState.a = message == WM_KEYDOWN; break;
			case KEY_D:
				cameraMoveState.d = message == WM_KEYDOWN; break;
			case KEY_CTRL:
				cameraMoveState.ctrl = message == WM_KEYDOWN; break;
			case KEY_SPACE:
				cameraMoveState.space = message == WM_KEYDOWN; break;
			case KEY_UP: {
                if (message == WM_KEYDOWN) {
                    OnUpDown();
                }
                cameraMoveState.up = message == WM_KEYDOWN;
            } break;
			case KEY_DOWN: {
                if (message == WM_KEYDOWN) {
                    OnDownDown();
                }
                cameraMoveState.down = message == WM_KEYDOWN;
            } break;
			case KEY_LEFT: {
                if (message == WM_KEYDOWN) {
                    OnLeftDown();
                }
                cameraMoveState.left = message == WM_KEYDOWN;
            } break;
			case KEY_RIGHT: {
                if (message == WM_KEYDOWN) {
                    OnRightDown();
                }
                cameraMoveState.right = message == WM_KEYDOWN;
            } break;
			case KEY_Q:
				cameraMoveState.q = message == WM_KEYDOWN; break;
			case KEY_E:
				cameraMoveState.e = message == WM_KEYDOWN; break;
			case KEY_I:
				cameraMoveState.i = message == WM_KEYDOWN; break;
			case KEY_K:
				cameraMoveState.k = message == WM_KEYDOWN; break;
			case KEY_J:
				cameraMoveState.j = message == WM_KEYDOWN; break;
			case KEY_L:
				cameraMoveState.l = message == WM_KEYDOWN; break;
			case KEY_R:
                cameraMoveState.r = message == WM_KEYDOWN; break;
            case KEY_F: if (message == WM_KEYDOWN) SwitchCameraMode(); break;
            case KEY_V: if (message == WM_KEYDOWN) SwitchCameraSubMode(); break;
                // 手柄操作响应
                case BTN_A:
                    cameraMoveState.a_button = message == WM_KEYDOWN;
                    if (message == WM_KEYDOWN) JAKeyDown();
                    break;
                case BTN_B:
                    cameraMoveState.b_button = message == WM_KEYDOWN;
                    if (message == WM_KEYDOWN) JBKeyDown();
                    break;
                case BTN_X:
                    cameraMoveState.x_button = message == WM_KEYDOWN;
                    if (message == WM_KEYDOWN) JXKeyDown();
                    break;
                case BTN_Y:
                    cameraMoveState.y_button = message == WM_KEYDOWN;
                    if (message == WM_KEYDOWN) JYKeyDown();
                    break;
                case BTN_LB:
                    cameraMoveState.lb_button = message == WM_KEYDOWN;
                    break;
                case BTN_RB:
                    cameraMoveState.rb_button = message == WM_KEYDOWN;
                    break;
                case BTN_THUMBL:
                    cameraMoveState.thumb_l_button = message == WM_KEYDOWN;
                    break;
                case BTN_THUMBR:
                    cameraMoveState.thumb_r_button = message == WM_KEYDOWN;
                    break;
                case BTN_SELECT:
                    cameraMoveState.select_button = message == WM_KEYDOWN;
                    if (message == WM_KEYDOWN) JSelectKeyDown();
                    break;
                case BTN_START:
                    cameraMoveState.start_button = message == WM_KEYDOWN;
                    if (message == WM_KEYDOWN) JStartKeyDown();
                    break;
                case BTN_SHARE:
                    cameraMoveState.share_button = message == WM_KEYDOWN;
                    break;
                case BTN_XBOX:
                    cameraMoveState.xbox_button = message == WM_KEYDOWN;
                    break;

			default: break;
			}
		}
	}

    void
    on_cam_rawinput_joystick(JoystickEvent event) {
        int message = event.getMessage();
        float leftStickX = event.getLeftStickX();
        float leftStickY = event.getLeftStickY();
        float rightStickX = event.getRightStickX();
        float rightStickY = event.getRightStickY();
        float leftTrigger = event.getLeftTrigger();
        float rightTrigger = event.getRightTrigger();
        float hatX = event.getHatX();
        float hatY = event.getHatY();

        cameraMoveState.thumb_l_right = (std::abs(leftStickX) > 0.1f) ? leftStickX : 0;
        cameraMoveState.thumb_l_down = (std::abs(leftStickY) > 0.1f) ? leftStickY : 0;
        cameraMoveState.thumb_r_right = (std::abs(rightStickX) > 0.1f) ? rightStickX : 0;
        cameraMoveState.thumb_r_down = (std::abs(rightStickY) > 0.1f) ? rightStickY : 0;
        cameraMoveState.lt_button = (std::abs(leftTrigger) > 0.1f) ? leftTrigger : 0;
        cameraMoveState.rt_button = (std::abs(rightTrigger) > 0.1f) ? rightTrigger : 0;
        cameraMoveState.dpad_up = hatY == -1.0f;
        cameraMoveState.dpad_down = hatY == 1.0f;
        cameraMoveState.dpad_left = hatX == -1.0f;
        cameraMoveState.dpad_right = hatX == 1.0f;

        if (cameraMoveState.dpad_down) {
            JDadDown();
        }

//        LinkuraLocal::Log::InfoFmt(
//                "Motion event: action=%d, leftStickX=%.2f, leftStickY=%.2f, rightStickX=%.2f, rightStickY=%.2f, leftTrigger=%.2f, rightTrigger=%.2f, hatX=%.2f, hatY=%.2f",
//                message, leftStickX, leftStickY, rightStickX, rightStickY, leftTrigger,
//                rightTrigger, hatX, hatY);
    }

    void on_cam_network_input(float leftStickX, float leftStickY, float rightStickX, float rightStickY,
                              float leftTrigger, float leftGrip, float rightTrigger, float rightGrip,
                              float yaw, float pitch, float roll, float hmdPosX, float hmdPosY, float hmdPosZ,
                              int buttons, int flags, float ipdMeters, float hmdVerticalFovDegrees,
                              float leftEyeAngleLeftRadians, float leftEyeAngleRightRadians,
                              float leftEyeAngleUpRadians, float leftEyeAngleDownRadians,
                              float rightEyeAngleLeftRadians, float rightEyeAngleRightRadians,
                              float rightEyeAngleUpRadians, float rightEyeAngleDownRadians) {
        constexpr float minFovAngle = -1.55f;
        constexpr float maxFovAngle = 1.55f;
        const float fallbackHalfVerticalRadians = std::clamp(hmdVerticalFovDegrees, 20.0f, 170.0f) * 0.00872664625f;
        auto clampFov = [&](float value, float fallbackValue) {
            if (!std::isfinite(value)) {
                return fallbackValue;
            }
            return std::clamp(value, minFovAngle, maxFovAngle);
        };

        std::lock_guard<std::mutex> lock(networkCameraInputMutex);
        networkCameraInputState.leftStickX = std::clamp(leftStickX, -1.0f, 1.0f);
        networkCameraInputState.leftStickY = std::clamp(leftStickY, -1.0f, 1.0f);
        networkCameraInputState.rightStickX = std::clamp(rightStickX, -1.0f, 1.0f);
        networkCameraInputState.rightStickY = std::clamp(rightStickY, -1.0f, 1.0f);
        networkCameraInputState.leftTrigger = std::clamp(leftTrigger, 0.0f, 1.0f);
        networkCameraInputState.leftGrip = std::clamp(leftGrip, 0.0f, 1.0f);
        networkCameraInputState.rightTrigger = std::clamp(rightTrigger, 0.0f, 1.0f);
        networkCameraInputState.rightGrip = std::clamp(rightGrip, 0.0f, 1.0f);
        networkCameraInputState.yaw = yaw;
        networkCameraInputState.pitch = pitch;
        networkCameraInputState.roll = roll;
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
        networkCameraInputState.flags = static_cast<uint32_t>(flags);
        static auto lastTelemetryLogAt = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTelemetryLogAt).count() >= 1000) {
            lastTelemetryLogAt = now;
            LinkuraLocal::Log::InfoFmt(
                "Network input telemetry: ipd=%.4f vFov=%.2f leftFov=(%.4f,%.4f,%.4f,%.4f) rightFov=(%.4f,%.4f,%.4f,%.4f) baseFov=%.2f yaw=%.3f pitch=%.3f roll=%.3f flags=%u buttons=%u",
                networkCameraInputState.ipdMeters,
                networkCameraInputState.hmdVerticalFovDegrees,
                networkCameraInputState.leftEyeAngleLeftRadians,
                networkCameraInputState.leftEyeAngleRightRadians,
                networkCameraInputState.leftEyeAngleUpRadians,
                networkCameraInputState.leftEyeAngleDownRadians,
                networkCameraInputState.rightEyeAngleLeftRadians,
                networkCameraInputState.rightEyeAngleRightRadians,
                networkCameraInputState.rightEyeAngleUpRadians,
                networkCameraInputState.rightEyeAngleDownRadians,
                baseCamera.fov,
                networkCameraInputState.yaw,
                networkCameraInputState.pitch,
                networkCameraInputState.roll,
                networkCameraInputState.flags,
                networkCameraInputState.buttons
            );
        }

        if (networkCameraInputState.leftStickX == 0.0f &&
            networkCameraInputState.leftStickY == 0.0f &&
            networkCameraInputState.rightStickX == 0.0f &&
            networkCameraInputState.rightStickY == 0.0f &&
            networkCameraInputState.leftTrigger == 0.0f &&
            networkCameraInputState.leftGrip == 0.0f &&
            networkCameraInputState.rightTrigger == 0.0f &&
            networkCameraInputState.rightGrip == 0.0f &&
            networkCameraInputState.yaw == 0.0f &&
            networkCameraInputState.pitch == 0.0f &&
            networkCameraInputState.roll == 0.0f &&
            networkCameraInputState.hmdPosX == 0.0f &&
            networkCameraInputState.hmdPosY == 0.0f &&
            networkCameraInputState.hmdPosZ == 0.0f &&
            networkCameraInputState.buttons == 0) {
            networkCameraInputState.hasPrevHmdPose = false;
            networkCameraInputState.hasPrevHeadAngle = false;
            networkCameraInputState.filteredYawDelta = 0.0f;
            networkCameraInputState.filteredPitchDelta = 0.0f;
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

	void initCameraSettings() {
		reset_camera();
		cameraRawInputThread();
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
