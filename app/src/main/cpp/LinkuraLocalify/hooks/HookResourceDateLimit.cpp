#include "../HookMain.h"
#include "../http-mock/HttpMock.hpp"
#include <unordered_map>

namespace LinkuraLocal::HookResourceDateLimit {
    namespace {
        constexpr auto QuestTopPath = "/v1/out_quest_live/get_quest_top";
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
        Il2cppUtils::FieldInfo* grandPrixIdField = nullptr;
        bool loggedMissingGrandPrixIdField = false;
        bool loggedActiveGrandPrixIdError = false;
        bool activeGrandPrixIdLoaded = false;
        int32_t activeGrandPrixId = 0;
        using OpenMusicSelectEventListPopupAsync = void* (*)(void*, void*);
        OpenMusicSelectEventListPopupAsync openMusicSelectEventListPopupAsync = nullptr;
        Il2cppUtils::MethodInfo* openMusicSelectEventListPopupAsyncMethod = nullptr;

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

        Il2cppUtils::FieldInfo* GetGrandPrixIdField(void* record) {
            if (!grandPrixIdField && record) {
                grandPrixIdField = Il2cppUtils::il2cpp_class_get_field_from_name(
                    Il2cppUtils::get_class_from_instance(record),
                    "<Id>k__BackingField"
                );
            }
            if (!grandPrixIdField && !loggedMissingGrandPrixIdField) {
                Log::Error("GrandPrixRecord id field was not found");
                loggedMissingGrandPrixIdField = true;
            }
            return grandPrixIdField;
        }

        int32_t GetGrandPrixRecordId(void* record) {
            auto field = GetGrandPrixIdField(record);
            if (!record || !field) {
                return 0;
            }
            return Il2cppUtils::ClassGetFieldValue<int32_t>(record, field);
        }

        void LoadActiveGrandPrixId() {
            if (activeGrandPrixIdLoaded) {
                return;
            }
            activeGrandPrixIdLoaded = true;
            activeGrandPrixId = 0;

            if (Config::apiMockBaseUrl.empty()) {
                if (!loggedActiveGrandPrixIdError) {
                    Log::Error("apiMockBaseUrl is empty while resolving active grand prix id");
                    loggedActiveGrandPrixIdError = true;
                }
                return;
            }

            const auto body = HttpMock::FetchSelfhostApiBody(
                Config::apiMockBaseUrl,
                QuestTopPath,
                "{}"
            );
            const auto response = nlohmann::json::parse(body, nullptr, false);
            if (
                !response.is_object() ||
                !response.contains("grand_prix_id") ||
                !response["grand_prix_id"].is_number_integer()
            ) {
                if (!loggedActiveGrandPrixIdError) {
                    Log::ErrorFmt("Failed to resolve active grand prix id from %s", QuestTopPath);
                    loggedActiveGrandPrixIdError = true;
                }
                return;
            }

            activeGrandPrixId = response["grand_prix_id"].get<int32_t>();
            Log::InfoFmt("Resolved active grand prix id: %d", activeGrandPrixId);
        }

        void RefreshActiveGrandPrixId() {
            activeGrandPrixIdLoaded = false;
            LoadActiveGrandPrixId();
        }

        bool IsActiveGrandPrixRecord(void* record) {
            if (!record) {
                return false;
            }
            LoadActiveGrandPrixId();
            if (activeGrandPrixId <= 0) {
                return false;
            }
            return GetGrandPrixRecordId(record) == activeGrandPrixId;
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

    DEFINE_HOOK(
        bool,
        MusicSelectFooter_EventLockValuePredicate,
        (void* self, bool eventLocked, void* method)
    ) {
        if (Config::disableResourceDateLimit) {
            return false;
        }
        return MusicSelectFooter_EventLockValuePredicate_Orig(self, eventLocked, method);
    }

    DEFINE_HOOK(
        bool,
        MusicSelectFooter_LiveGrandPrixOpenPredicate,
        (void* self, void* record, void* method)
    ) {
        if (Config::disableResourceDateLimit) {
            return record != nullptr;
        }
        return MusicSelectFooter_LiveGrandPrixOpenPredicate_Orig(self, record, method);
    }

    DEFINE_HOOK(
        bool,
        MusicSelectFooter_OnClickEventButtonAsObservableValue,
        (void* self, void* unit, void* method)
    ) {
        if (Config::disableResourceDateLimit) {
            return false;
        }
        return MusicSelectFooter_OnClickEventButtonAsObservableValue_Orig(self, unit, method);
    }

    DEFINE_HOOK(
        void,
        MusicSelectSceneController_EventButtonSubscriber,
        (void* self, bool eventLocked, void* method)
    ) {
        if (Config::disableResourceDateLimit && openMusicSelectEventListPopupAsync && openMusicSelectEventListPopupAsyncMethod) {
            RefreshActiveGrandPrixId();
            openMusicSelectEventListPopupAsync(self, openMusicSelectEventListPopupAsyncMethod);
            return;
        }
        MusicSelectSceneController_EventButtonSubscriber_Orig(
            self,
            Config::disableResourceDateLimit ? false : eventLocked,
            method
        );
    }

    DEFINE_HOOK(
        bool,
        MusicSelectSceneController_EventListGrandPrixOpenPredicate,
        (void* self, void* record, void* method)
    ) {
        if (Config::disableResourceDateLimit) {
            return IsActiveGrandPrixRecord(record);
        }
        return MusicSelectSceneController_EventListGrandPrixOpenPredicate_Orig(self, record, method);
    }

    DEFINE_HOOK(
        void,
        MusicSelectEventListCell_Initialize,
        (void* self, int32_t rhythmGameEventSeriesId, bool isLock, void* eventIcon, void* method)
    ) {
        MusicSelectEventListCell_Initialize_Orig(
            self,
            rhythmGameEventSeriesId,
            Config::disableResourceDateLimit ? false : isLock,
            eventIcon,
            method
        );
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

        auto musicSelectFooterKlass = Il2cppUtils::GetClassIl2cpp(
            "Assembly-CSharp.dll",
            "RhythmGame.MusicSelect",
            "MusicSelectFooter"
        );
        auto musicSelectFooterHelperKlass = Il2cppUtils::find_nested_class_from_name(
            musicSelectFooterKlass,
            "<>c"
        );
        auto musicSelectSceneControllerKlass = Il2cppUtils::GetClassIl2cpp(
            "Assembly-CSharp.dll",
            "RhythmGame.MusicSelect",
            "MusicSelectSceneController"
        );
        auto musicSelectSceneControllerHelperKlass = Il2cppUtils::find_nested_class_from_name(
            musicSelectSceneControllerKlass,
            "<>c"
        );
        auto musicSelectEventListCellKlass = Il2cppUtils::GetClassIl2cpp(
            "Assembly-CSharp.dll",
            "RhythmGame.MusicSelect",
            "MusicSelectEventListCell"
        );
        auto eventLockValuePredicate = Il2cppUtils::GetMethodIl2cpp(
            musicSelectFooterHelperKlass,
            "<SetEventLock>b__10_0",
            1
        );
        auto liveGrandPrixOpenPredicate = Il2cppUtils::GetMethodIl2cpp(
            musicSelectFooterHelperKlass,
            "<SetEventLock>b__10_1",
            1
        );
        auto onClickEventButtonAsObservableValue = Il2cppUtils::GetMethodIl2cpp(
            musicSelectFooterKlass,
            "<OnClickEventButtonAsObservable>b__6_0",
            1
        );
        auto eventButtonSubscriber = Il2cppUtils::GetMethodIl2cpp(
            musicSelectSceneControllerKlass,
            "<ConnectReactiveStreams>b__13_9",
            1
        );
        openMusicSelectEventListPopupAsyncMethod = Il2cppUtils::GetMethodIl2cpp(
            musicSelectSceneControllerKlass,
            "OpenMusicSelectEventListPopupAsync",
            0
        );
        openMusicSelectEventListPopupAsync = openMusicSelectEventListPopupAsyncMethod
            ? reinterpret_cast<OpenMusicSelectEventListPopupAsync>(
                  openMusicSelectEventListPopupAsyncMethod->methodPointer
              )
            : nullptr;
        auto eventListGrandPrixOpenPredicate = Il2cppUtils::GetMethodIl2cpp(
            musicSelectSceneControllerHelperKlass,
            "<OpenMusicSelectEventListPopupAsync>b__23_1",
            1
        );
        auto eventListCellInitialize = Il2cppUtils::GetMethodIl2cpp(
            musicSelectEventListCellKlass,
            "Initialize",
            3
        );
        ADD_HOOK(
            MusicSelectFooter_EventLockValuePredicate,
            eventLockValuePredicate ? eventLockValuePredicate->methodPointer : 0
        );
        ADD_HOOK(
            MusicSelectFooter_LiveGrandPrixOpenPredicate,
            liveGrandPrixOpenPredicate ? liveGrandPrixOpenPredicate->methodPointer : 0
        );
        ADD_HOOK(
            MusicSelectFooter_OnClickEventButtonAsObservableValue,
            onClickEventButtonAsObservableValue ? onClickEventButtonAsObservableValue->methodPointer : 0
        );
        ADD_HOOK(
            MusicSelectSceneController_EventButtonSubscriber,
            eventButtonSubscriber ? eventButtonSubscriber->methodPointer : 0
        );
        ADD_HOOK(
            MusicSelectSceneController_EventListGrandPrixOpenPredicate,
            eventListGrandPrixOpenPredicate ? eventListGrandPrixOpenPredicate->methodPointer : 0
        );
        ADD_HOOK(
            MusicSelectEventListCell_Initialize,
            eventListCellInitialize ? eventListCellInitialize->methodPointer : 0
        );
    }
}
