#pragma once

namespace L4Camera {
    void SyncNetworkControllerNavigationStateFromRuntime();
    void ApplyNetworkControllerNavigationState();
    void CycleNetworkControllerMajorView();
    void CycleCurrentCharacterCameraType();
    void SelectNextCharacterFromController();
    void SelectPreviousCharacterFromController();
}
