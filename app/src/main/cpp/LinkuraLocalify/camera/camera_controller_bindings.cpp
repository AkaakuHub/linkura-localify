#include "camera_controller_bindings.hpp"

#include "camera_internal.hpp"
#include "../BaseDefine.h"

#include <cmath>

namespace L4Camera {
    namespace {
        struct NetworkControllerNavigationState {
            bool hasActiveViewState = false;
            LinkuraLocal::HookCamera::FesLiveViewType currentMajorView =
                LinkuraLocal::HookCamera::FesLiveViewType::Unknown;
        };

        NetworkControllerNavigationState networkControllerNavigationState;

        LinkuraLocal::HookCamera::FesLiveViewType GetNextMajorView(
            LinkuraLocal::HookCamera::FesLiveViewType currentView
        ) {
            using FesLiveViewType = LinkuraLocal::HookCamera::FesLiveViewType;

            switch (currentView) {
                case FesLiveViewType::DynamicView:
                    return FesLiveViewType::ArenaView;
                case FesLiveViewType::ArenaView:
                    return FesLiveViewType::StandView;
                case FesLiveViewType::StandView:
                    return FesLiveViewType::SchoolIdle;
                case FesLiveViewType::SchoolIdle:
                    return FesLiveViewType::DynamicView;
                default:
                    return FesLiveViewType::DynamicView;
            }
        }
    }

    void SyncNetworkControllerNavigationStateFromRuntime() {
        using FesLiveViewType = LinkuraLocal::HookCamera::FesLiveViewType;

        const auto activeView = LinkuraLocal::HookCamera::GetActiveFesLiveView();
        if (activeView == FesLiveViewType::Unknown) {
            return;
        }

        networkControllerNavigationState.hasActiveViewState = true;
        networkControllerNavigationState.currentMajorView = activeView;
    }

    void ApplyNetworkControllerNavigationState() {
        if (!networkControllerNavigationState.hasActiveViewState) {
            return;
        }

        const auto activeView = LinkuraLocal::HookCamera::GetActiveFesLiveView();
        if (activeView != networkControllerNavigationState.currentMajorView) {
            LinkuraLocal::HookCamera::RequestFesLiveViewSwitch(
                networkControllerNavigationState.currentMajorView
            );
        }
    }

    void CycleNetworkControllerMajorView() {
        using FesLiveViewType = LinkuraLocal::HookCamera::FesLiveViewType;

        SyncNetworkControllerNavigationStateFromRuntime();
        if (!networkControllerNavigationState.hasActiveViewState) {
            return;
        }

        const auto activeView = LinkuraLocal::HookCamera::GetActiveFesLiveView();
        const auto currentView =
            activeView == FesLiveViewType::Unknown
                ? networkControllerNavigationState.currentMajorView
                : activeView;

        if (currentView == FesLiveViewType::Unknown) {
            networkControllerNavigationState.currentMajorView = FesLiveViewType::DynamicView;
            return;
        }

        networkControllerNavigationState.currentMajorView = GetNextMajorView(currentView);
    }

    void CycleCurrentCharacterCameraType() {
        if (LinkuraLocal::HookCamera::GetActiveFesLiveView()
            != LinkuraLocal::HookCamera::FesLiveViewType::StandView) {
            return;
        }

        SwitchCameraMode();
    }

    void SelectNextCharacterFromController() {
        if (LinkuraLocal::HookCamera::GetActiveFesLiveView()
                == LinkuraLocal::HookCamera::FesLiveViewType::StandView
            && GetCameraMode() == CameraMode::FREE) {
            SwitchCameraMode();
        }

        OnRightDown();
    }

    void SelectPreviousCharacterFromController() {
        if (LinkuraLocal::HookCamera::GetActiveFesLiveView()
                == LinkuraLocal::HookCamera::FesLiveViewType::StandView
            && GetCameraMode() == CameraMode::FREE) {
            SwitchCameraMode();
        }

        OnLeftDown();
    }

    void on_cam_rawinput_keyboard(int message, int key) {
        if (message != WM_KEYDOWN && message != WM_KEYUP) {
            return;
        }

        switch (key) {
            case KEY_W:
                cameraMoveState.w = message == WM_KEYDOWN;
                break;
            case KEY_S:
                cameraMoveState.s = message == WM_KEYDOWN;
                break;
            case KEY_A:
                cameraMoveState.a = message == WM_KEYDOWN;
                break;
            case KEY_D:
                cameraMoveState.d = message == WM_KEYDOWN;
                break;
            case KEY_CTRL:
                cameraMoveState.ctrl = message == WM_KEYDOWN;
                break;
            case KEY_SPACE:
                cameraMoveState.space = message == WM_KEYDOWN;
                break;
            case KEY_UP:
                if (message == WM_KEYDOWN) {
                    OnUpDown();
                }
                cameraMoveState.up = message == WM_KEYDOWN;
                break;
            case KEY_DOWN:
                if (message == WM_KEYDOWN) {
                    OnDownDown();
                }
                cameraMoveState.down = message == WM_KEYDOWN;
                break;
            case KEY_LEFT:
                if (message == WM_KEYDOWN) {
                    OnLeftDown();
                }
                cameraMoveState.left = message == WM_KEYDOWN;
                break;
            case KEY_RIGHT:
                if (message == WM_KEYDOWN) {
                    OnRightDown();
                }
                cameraMoveState.right = message == WM_KEYDOWN;
                break;
            case KEY_Q:
                cameraMoveState.q = message == WM_KEYDOWN;
                break;
            case KEY_E:
                cameraMoveState.e = message == WM_KEYDOWN;
                break;
            case KEY_I:
                cameraMoveState.i = message == WM_KEYDOWN;
                break;
            case KEY_K:
                cameraMoveState.k = message == WM_KEYDOWN;
                break;
            case KEY_J:
                cameraMoveState.j = message == WM_KEYDOWN;
                break;
            case KEY_L:
                cameraMoveState.l = message == WM_KEYDOWN;
                break;
            case KEY_R:
                cameraMoveState.r = message == WM_KEYDOWN;
                break;
            case KEY_F:
                if (message == WM_KEYDOWN) {
                    SwitchCameraMode();
                }
                break;
            case KEY_V:
                if (message == WM_KEYDOWN) {
                    SwitchCameraSubMode();
                }
                break;
            case BTN_A:
                cameraMoveState.a_button = message == WM_KEYDOWN;
                if (message == WM_KEYDOWN) {
                    JAKeyDown();
                }
                break;
            case BTN_B:
                cameraMoveState.b_button = message == WM_KEYDOWN;
                if (message == WM_KEYDOWN) {
                    JBKeyDown();
                }
                break;
            case BTN_X:
                cameraMoveState.x_button = message == WM_KEYDOWN;
                if (message == WM_KEYDOWN) {
                    JXKeyDown();
                }
                break;
            case BTN_Y:
                cameraMoveState.y_button = message == WM_KEYDOWN;
                if (message == WM_KEYDOWN) {
                    JYKeyDown();
                }
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
                if (message == WM_KEYDOWN) {
                    JSelectKeyDown();
                }
                break;
            case BTN_START:
                cameraMoveState.start_button = message == WM_KEYDOWN;
                if (message == WM_KEYDOWN) {
                    JStartKeyDown();
                }
                break;
            case BTN_SHARE:
                cameraMoveState.share_button = message == WM_KEYDOWN;
                break;
            case BTN_XBOX:
                cameraMoveState.xbox_button = message == WM_KEYDOWN;
                break;
            default:
                break;
        }
    }

    void on_cam_rawinput_joystick(JoystickEvent event) {
        const float leftStickX = event.getLeftStickX();
        const float leftStickY = event.getLeftStickY();
        const float rightStickX = event.getRightStickX();
        const float rightStickY = event.getRightStickY();
        const float leftTrigger = event.getLeftTrigger();
        const float rightTrigger = event.getRightTrigger();
        const float hatX = event.getHatX();
        const float hatY = event.getHatY();

        cameraMoveState.thumb_l_right = (std::abs(leftStickX) > 0.1f) ? leftStickX : 0.0f;
        cameraMoveState.thumb_l_down = (std::abs(leftStickY) > 0.1f) ? leftStickY : 0.0f;
        cameraMoveState.thumb_r_right = (std::abs(rightStickX) > 0.1f) ? rightStickX : 0.0f;
        cameraMoveState.thumb_r_down = (std::abs(rightStickY) > 0.1f) ? rightStickY : 0.0f;
        cameraMoveState.lt_button = (std::abs(leftTrigger) > 0.1f) ? leftTrigger : 0.0f;
        cameraMoveState.rt_button = (std::abs(rightTrigger) > 0.1f) ? rightTrigger : 0.0f;
        cameraMoveState.dpad_up = hatY == -1.0f;
        cameraMoveState.dpad_down = hatY == 1.0f;
        cameraMoveState.dpad_left = hatX == -1.0f;
        cameraMoveState.dpad_right = hatX == 1.0f;

        if (cameraMoveState.dpad_down) {
            JDadDown();
        }
    }
}
