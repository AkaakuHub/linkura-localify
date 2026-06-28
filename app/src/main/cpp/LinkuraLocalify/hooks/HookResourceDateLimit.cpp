#include "../HookMain.h"
#include <unordered_map>
#include <vector>

namespace LinkuraLocal::HookResourceDateLimit {
    namespace {
        constexpr int64_t AlwaysOpenStartDateTime = 621355968000000000LL;
        constexpr int64_t AlwaysOpenEndDateTime = 3155378975999999999LL;

        struct DateRange {
            int64_t startTime;
            int64_t endTime;
        };

        thread_local bool isBuildingGachaInfoList = false;
        thread_local std::unordered_map<void*, DateRange> patchedGachaSeriesRecords;

        Il2cppUtils::FieldInfo* GetGachaSeriesStartTimeField(void* record) {
            static Il2cppUtils::FieldInfo* field = nullptr;
            if (!field && record) {
                field = Il2cppUtils::il2cpp_class_get_field_from_name(
                    Il2cppUtils::get_class_from_instance(record),
                    "<StartTime>k__BackingField"
                );
            }
            return field;
        }

        Il2cppUtils::FieldInfo* GetGachaSeriesEndTimeField(void* record) {
            static Il2cppUtils::FieldInfo* field = nullptr;
            if (!field && record) {
                field = Il2cppUtils::il2cpp_class_get_field_from_name(
                    Il2cppUtils::get_class_from_instance(record),
                    "<EndTime>k__BackingField"
                );
            }
            return field;
        }

        void OpenGachaSeriesDateRange(void* record) {
            if (!record || patchedGachaSeriesRecords.contains(record)) {
                return;
            }

            auto startTimeField = GetGachaSeriesStartTimeField(record);
            auto endTimeField = GetGachaSeriesEndTimeField(record);
            if (!startTimeField || !endTimeField) {
                Log::Error("GachaSeriesRecord date fields were not found");
                return;
            }

            patchedGachaSeriesRecords.emplace(record, DateRange{
                Il2cppUtils::ClassGetFieldValue<int64_t>(record, startTimeField),
                Il2cppUtils::ClassGetFieldValue<int64_t>(record, endTimeField),
            });

            Il2cppUtils::ClassSetFieldValue<int64_t>(record, startTimeField, AlwaysOpenStartDateTime);
            Il2cppUtils::ClassSetFieldValue<int64_t>(record, endTimeField, AlwaysOpenEndDateTime);
        }

        void RestoreGachaSeriesDateRanges() {
            auto startTimeField = GetGachaSeriesStartTimeField(nullptr);
            auto endTimeField = GetGachaSeriesEndTimeField(nullptr);
            if (startTimeField && endTimeField) {
                for (auto& [record, dateRange] : patchedGachaSeriesRecords) {
                    Il2cppUtils::ClassSetFieldValue<int64_t>(record, startTimeField, dateRange.startTime);
                    Il2cppUtils::ClassSetFieldValue<int64_t>(record, endTimeField, dateRange.endTime);
                }
            }
            patchedGachaSeriesRecords.clear();
        }
    }

    DEFINE_HOOK(
        void,
        GachaSceneController_SetGachaInfoList,
        (void* self, void* gachaSeriesList, void* selectSeriesList, bool isReserve, void* method)
    ) {
        if (!Config::disableResourceDateLimit) {
            GachaSceneController_SetGachaInfoList_Orig(self, gachaSeriesList, selectSeriesList, isReserve, method);
            return;
        }

        isBuildingGachaInfoList = true;
        patchedGachaSeriesRecords.clear();
        GachaSceneController_SetGachaInfoList_Orig(self, gachaSeriesList, selectSeriesList, isReserve, method);
        isBuildingGachaInfoList = false;
        RestoreGachaSeriesDateRanges();
    }

    DEFINE_HOOK(
        void*,
        GachaSeriesMaster_Fetch,
        (void* self, int64_t id, void* method)
    ) {
        auto record = GachaSeriesMaster_Fetch_Orig(self, id, method);
        if (Config::disableResourceDateLimit && isBuildingGachaInfoList) {
            OpenGachaSeriesDateRange(record);
        }
        return record;
    }

    void Install(HookInstaller* hookInstaller) {
        ADD_HOOK(
            GachaSceneController_SetGachaInfoList,
            Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Tecotec", "GachaSceneController", "SetGachaInfoList")
        );
        ADD_HOOK(
            GachaSeriesMaster_Fetch,
            Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "GachaSeriesMaster", "Fetch")
        );
    }
}
