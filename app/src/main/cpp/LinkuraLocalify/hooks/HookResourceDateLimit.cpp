#include "../HookMain.h"
#include <unordered_map>

namespace LinkuraLocal::HookResourceDateLimit {
    namespace {
        constexpr int64_t AlwaysOpenStartDateTime = 621355968000000000LL;
        constexpr int64_t AlwaysOpenEndDateTime = 3155378975999999999LL;

        struct DateRange {
            int64_t startTime;
            int64_t endTime;
        };

        std::unordered_map<void*, DateRange> patchedGachaSeriesRecords;
        bool loggedMissingDateFields = false;

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
            if (!record) {
                return;
            }

            auto startTimeField = GetGachaSeriesStartTimeField(record);
            auto endTimeField = GetGachaSeriesEndTimeField(record);
            if (!startTimeField || !endTimeField) {
                if (!loggedMissingDateFields) {
                    Log::Error("GachaSeriesRecord date fields were not found");
                    loggedMissingDateFields = true;
                }
                return;
            }

            if (!patchedGachaSeriesRecords.contains(record)) {
                patchedGachaSeriesRecords.emplace(record, DateRange{
                    Il2cppUtils::ClassGetFieldValue<int64_t>(record, startTimeField),
                    Il2cppUtils::ClassGetFieldValue<int64_t>(record, endTimeField),
                });
            }

            Il2cppUtils::ClassSetFieldValue<int64_t>(record, startTimeField, AlwaysOpenStartDateTime);
            Il2cppUtils::ClassSetFieldValue<int64_t>(record, endTimeField, AlwaysOpenEndDateTime);
        }

        void RestoreGachaSeriesDateRange(void* record) {
            auto patchedRecord = patchedGachaSeriesRecords.find(record);
            if (patchedRecord == patchedGachaSeriesRecords.end()) {
                return;
            }

            auto startTimeField = GetGachaSeriesStartTimeField(record);
            auto endTimeField = GetGachaSeriesEndTimeField(record);
            if (startTimeField && endTimeField) {
                Il2cppUtils::ClassSetFieldValue<int64_t>(record, startTimeField, patchedRecord->second.startTime);
                Il2cppUtils::ClassSetFieldValue<int64_t>(record, endTimeField, patchedRecord->second.endTime);
            }
            patchedGachaSeriesRecords.erase(patchedRecord);
        }
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

    DEFINE_HOOK(
        void*,
        GachaSeriesMaster_Fetch,
        (void* self, int64_t id, void* method)
    ) {
        auto record = GachaSeriesMaster_Fetch_Orig(self, id, method);
        if (Config::disableResourceDateLimit) {
            OpenGachaSeriesDateRange(record);
        } else {
            RestoreGachaSeriesDateRange(record);
        }
        return record;
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
        ADD_HOOK(
            GachaSeriesMaster_Fetch,
            Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "GachaSeriesMaster", "Fetch")
        );
    }
}
