#pragma once

#include "camera.hpp"

namespace L4Camera {
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

        void resetAll();
    };

    extern UnityResolve::UnityType::Vector2 followLookAtOffset;
    extern float offsetMoveStep;
    extern float l_sensitivity;
    extern float r_sensitivity;
    extern bool showToast;
    extern CameraMoveState cameraMoveState;

    void camera_forward(float multiplier);
    void camera_back(float multiplier);
    void camera_left(float multiplier);
    void camera_right(float multiplier);
    void camera_down(float multiplier);
    void camera_up(float multiplier);
    void cameraLookat_up(float mAngel, bool mouse = false);
    void cameraLookat_down(float mAngel, bool mouse = false);
    void cameraLookat_left(float mAngel);
    void cameraLookat_right(float mAngel);
    void changeCameraFOV(float value);
    void SwitchCameraMode();
    void SwitchCameraSubMode();
    void OnLeftDown();
    void OnRightDown();
    void OnUpDown();
    void OnDownDown();
    void ChangeLiveFollowCameraOffsetY(float value);
    void ChangeLiveFollowCameraOffsetX(const float value);
    void ShowToast(const char* text);
    void JLThumbRight(float value);
    void JLThumbDown(float value);
    void JRThumbRight(float value);
    void JRThumbDown(float value);
    void JDadUp();
    void JDadDown();
    void JDadLeft();
    void JDadRight();
    void JAKeyDown();
    void JBKeyDown();
    void JXKeyDown();
    void JYKeyDown();
    void JSelectKeyDown();
    void JStartKeyDown();
    void clampBaseCameraPitch();
}
