#include "../HookMain.h"

namespace LinkuraLocal::HookResourceDateLimit {
    namespace {
        constexpr int64_t AlwaysOpenStartDateTime = 621355968000000000LL;
        constexpr int64_t AlwaysOpenEndDateTime = 3155378975999999999LL;
    }

    DEFINE_HOOK(
        int64_t,
        GachaSeriesRecord_get_StartTime,
        (void* self, void* method)
    ) {
        if (Config::disableResourceDateLimit) {
            return AlwaysOpenStartDateTime;
        }
        return GachaSeriesRecord_get_StartTime_Orig(self, method);
    }

    DEFINE_HOOK(
        int64_t,
        GachaSeriesRecord_get_EndTime,
        (void* self, void* method)
    ) {
        if (Config::disableResourceDateLimit) {
            return AlwaysOpenEndDateTime;
        }
        return GachaSeriesRecord_get_EndTime_Orig(self, method);
    }

    void Install(HookInstaller* hookInstaller) {
        ADD_HOOK(
            GachaSeriesRecord_get_StartTime,
            Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "GachaSeriesRecord", "get_StartTime")
        );
        ADD_HOOK(
            GachaSeriesRecord_get_EndTime,
            Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "GachaSeriesRecord", "get_EndTime")
        );
    }
}
