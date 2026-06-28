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

        struct DateFieldAccess {
            const char* recordName;
            Il2cppUtils::FieldInfo* startTimeField;
            Il2cppUtils::FieldInfo* endTimeField;
            bool loggedMissingFields;
        };

        std::unordered_map<void*, DateRange> patchedDateRecords;
        DateFieldAccess gachaSeriesDateFields{"GachaSeriesRecord", nullptr, nullptr, false};
        DateFieldAccess grandPrixDateFields{"GrandPrixRecord", nullptr, nullptr, false};

        Il2cppUtils::FieldInfo* GetStartTimeField(void* record, DateFieldAccess& fields) {
            if (!fields.startTimeField && record) {
                fields.startTimeField = Il2cppUtils::il2cpp_class_get_field_from_name(
                    Il2cppUtils::get_class_from_instance(record),
                    "<StartTime>k__BackingField"
                );
            }
            return fields.startTimeField;
        }

        Il2cppUtils::FieldInfo* GetEndTimeField(void* record, DateFieldAccess& fields) {
            if (!fields.endTimeField && record) {
                fields.endTimeField = Il2cppUtils::il2cpp_class_get_field_from_name(
                    Il2cppUtils::get_class_from_instance(record),
                    "<EndTime>k__BackingField"
                );
            }
            return fields.endTimeField;
        }

        void OpenDateRange(void* record, DateFieldAccess& fields) {
            if (!record) {
                return;
            }

            auto startTimeField = GetStartTimeField(record, fields);
            auto endTimeField = GetEndTimeField(record, fields);
            if (!startTimeField || !endTimeField) {
                if (!fields.loggedMissingFields) {
                    Log::ErrorFmt("%s date fields were not found", fields.recordName);
                    fields.loggedMissingFields = true;
                }
                return;
            }

            if (!patchedDateRecords.contains(record)) {
                patchedDateRecords.emplace(record, DateRange{
                    Il2cppUtils::ClassGetFieldValue<int64_t>(record, startTimeField),
                    Il2cppUtils::ClassGetFieldValue<int64_t>(record, endTimeField),
                });
            }

            Il2cppUtils::ClassSetFieldValue<int64_t>(record, startTimeField, AlwaysOpenStartDateTime);
            Il2cppUtils::ClassSetFieldValue<int64_t>(record, endTimeField, AlwaysOpenEndDateTime);
        }

        void RestoreDateRange(void* record, DateFieldAccess& fields) {
            auto patchedRecord = patchedDateRecords.find(record);
            if (patchedRecord == patchedDateRecords.end()) {
                return;
            }

            auto startTimeField = GetStartTimeField(record, fields);
            auto endTimeField = GetEndTimeField(record, fields);
            if (startTimeField && endTimeField) {
                Il2cppUtils::ClassSetFieldValue<int64_t>(record, startTimeField, patchedRecord->second.startTime);
                Il2cppUtils::ClassSetFieldValue<int64_t>(record, endTimeField, patchedRecord->second.endTime);
            }
            patchedDateRecords.erase(patchedRecord);
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
            OpenDateRange(record, gachaSeriesDateFields);
        } else {
            RestoreDateRange(record, gachaSeriesDateFields);
        }
        return record;
    }

    DEFINE_HOOK(
        int64_t,
        GrandPrixRecord_get_StartTime,
        (void* self, void* method)
    ) {
        if (Config::disableResourceDateLimit) {
            return AlwaysOpenStartDateTime;
        }
        return GrandPrixRecord_get_StartTime_Orig(self, method);
    }

    DEFINE_HOOK(
        int64_t,
        GrandPrixRecord_get_EndTime,
        (void* self, void* method)
    ) {
        if (Config::disableResourceDateLimit) {
            return AlwaysOpenEndDateTime;
        }
        return GrandPrixRecord_get_EndTime_Orig(self, method);
    }

    DEFINE_HOOK(
        void*,
        GrandPrixMaster_Fetch,
        (void* self, int64_t id, void* method)
    ) {
        auto record = GrandPrixMaster_Fetch_Orig(self, id, method);
        if (Config::disableResourceDateLimit) {
            OpenDateRange(record, grandPrixDateFields);
        } else {
            RestoreDateRange(record, grandPrixDateFields);
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
        ADD_HOOK(
            GrandPrixRecord_get_StartTime,
            Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "GrandPrixRecord", "get_StartTime")
        );
        ADD_HOOK(
            GrandPrixRecord_get_EndTime,
            Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "GrandPrixRecord", "get_EndTime")
        );
        ADD_HOOK(
            GrandPrixMaster_Fetch,
            Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "GrandPrixMaster", "Fetch")
        );
    }
}
