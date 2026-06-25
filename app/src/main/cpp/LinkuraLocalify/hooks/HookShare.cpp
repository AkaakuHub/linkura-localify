#include "../HookMain.h"
#include "../Misc.hpp"
#include "../Local.h"
#include <re2/re2.h>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_set>

namespace LinkuraLocal::HookShare {
    namespace {
        std::mutex apiAuditContextMutex;
        std::string lastOfficialApiPath;
        std::string lastOfficialApiRequest;

        static bool IsRestSharpResponse(void* response) {
            if (!response) return false;

            const auto klass = Il2cppUtils::get_class_from_instance(response);
            if (!klass || !klass->namespaze || !klass->name) return false;
            if (std::string_view(klass->namespaze) != "RestSharp") return false;
            return std::string_view(klass->name).find("RestResponse") != std::string_view::npos
                || std::string_view(klass->name).find("Response") != std::string_view::npos;
        }

        static Il2cppUtils::MethodInfo* ResolveRestResponseGetHeaders() {
            static Il2cppUtils::MethodInfo* s_getHeadersMi = nullptr;
            static bool s_resolved = false;
            if (s_resolved) return s_getHeadersMi;
            s_resolved = true;

            s_getHeadersMi = Il2cppUtils::GetMethodIl2cpp("RestSharp.dll", "RestSharp", "RestResponseBase", "get_Headers", 0);
            if (!s_getHeadersMi) {
                s_getHeadersMi = Il2cppUtils::GetMethodIl2cpp("RestSharp", "RestSharp", "RestResponseBase", "get_Headers", 0);
            }
            return s_getHeadersMi;
        }

        static Il2cppUtils::MethodInfo* ResolveRestResponseGetter(const char* name) {
            auto method = Il2cppUtils::GetMethodIl2cpp("RestSharp.dll", "RestSharp", "RestResponseBase", name, 0);
            if (!method) {
                method = Il2cppUtils::GetMethodIl2cpp("RestSharp", "RestSharp", "RestResponseBase", name, 0);
            }
            return method;
        }

        static std::string LowercaseAscii(std::string value) {
            for (auto& ch : value) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
            return value;
        }

        static bool HasUppercaseAscii(std::string_view value) {
            for (const auto ch : value) {
                if (ch >= 'A' && ch <= 'Z') return true;
            }
            return false;
        }

        static void NormalizeRestResponseHeaderNamesIfPossible(void* response) {
            if (!IsRestSharpResponse(response)) return;

            auto getHeadersMi = ResolveRestResponseGetHeaders();
            if (!getHeadersMi) return;

            UnityResolve::ThreadAttach();

            using GetHeadersFn = void*(*)(void*, Il2cppUtils::MethodInfo*);
            auto headersObj = reinterpret_cast<GetHeadersFn>(getHeadersMi->methodPointer)(response, getHeadersMi);
            if (!headersObj) return;

            auto hdrKlass = Il2cppUtils::get_class_from_instance(headersObj);
            const bool isGenericList = hdrKlass
                                    && hdrKlass->namespaze
                                    && hdrKlass->name
                                    && std::string_view(hdrKlass->namespaze) == "System.Collections.Generic"
                                    && std::string_view(hdrKlass->name).find("List`1") != std::string_view::npos;
            if (!isGenericList) return;

            auto list = reinterpret_cast<UnityResolve::UnityType::List<void*>*>(headersObj);
            if (!list || !list->pList) return;

            static void* s_paramKlass = nullptr;
            static Il2cppUtils::MethodInfo* s_paramGetNameMi = nullptr;
            static Il2cppUtils::MethodInfo* s_paramSetNameMi = nullptr;
            static bool s_paramResolved = false;
            if (!s_paramResolved) {
                s_paramResolved = true;
                s_paramKlass = Il2cppUtils::GetClassIl2cpp("RestSharp.dll", "RestSharp", "Parameter");
                if (!s_paramKlass) s_paramKlass = Il2cppUtils::GetClassIl2cpp("RestSharp", "RestSharp", "Parameter");
                if (s_paramKlass) {
                    s_paramGetNameMi = Il2cppUtils::GetMethodIl2cpp(s_paramKlass, "get_Name", 0);
                    s_paramSetNameMi = Il2cppUtils::GetMethodIl2cpp(s_paramKlass, "set_Name", 1);
                }
            }
            if (!s_paramGetNameMi || !s_paramSetNameMi) return;

            auto items = list->pList;
            const int cap = static_cast<int>(items->max_length);
            const int n = (list->size < cap) ? list->size : cap;
            int changed = 0;
            for (int i = 0; i < n; ++i) {
                auto param = items->At(static_cast<unsigned>(i));
                if (!param) continue;

                using GetNameFn = Il2cppUtils::Il2CppString*(*)(void*, Il2cppUtils::MethodInfo*);
                auto nameStr = reinterpret_cast<GetNameFn>(s_paramGetNameMi->methodPointer)(param, s_paramGetNameMi);
                if (!nameStr) continue;

                auto name = nameStr->ToString();
                if (!HasUppercaseAscii(name)) continue;

                auto lowered = Il2cppUtils::Il2CppString::New(LowercaseAscii(std::move(name)));
                using SetNameFn = void(*)(void*, Il2cppUtils::Il2CppString*, Il2cppUtils::MethodInfo*);
                reinterpret_cast<SetNameFn>(s_paramSetNameMi->methodPointer)(param, lowered, s_paramSetNameMi);
                ++changed;
            }

            if (changed > 0 && (Config::dbgMode || Config::enableOfflineApiMock)) {
                Log::InfoFmt("[ApiClient_Deserialize] normalized response header names changed=%d", changed);
            }
        }

        static void DumpRestResponseHeadersIfPossible(void* response) {
            if (!IsRestSharpResponse(response)) return;
            const auto klass = Il2cppUtils::get_class_from_instance(response);

            static std::mutex s_mtx;
            static std::unordered_set<void*> s_dumped;
            {
                std::lock_guard<std::mutex> _l(s_mtx);
                if (s_dumped.find(response) != s_dumped.end()) return;
                s_dumped.insert(response);
                if (s_dumped.size() > 256) s_dumped.clear();
            }

            auto s_getHeadersMi = ResolveRestResponseGetHeaders();
            if (!s_getHeadersMi) return;

            // This hook may run on non-Unity threads.
            UnityResolve::ThreadAttach();

            using GetHeadersFn = void*(*)(void*, Il2cppUtils::MethodInfo*);
            auto headersObj = reinterpret_cast<GetHeadersFn>(s_getHeadersMi->methodPointer)(response, s_getHeadersMi);
            if (!headersObj) {
                Log::InfoFmt("[ApiClient_Deserialize] response headers=null response=%p klass=%s.%s",
                             response, klass->namespaze, klass->name);
                return;
            }

            Log::InfoFmt("[ApiClient_Deserialize] response headers dump begin response=%p headers=%p klass=%s.%s",
                         response, headersObj, klass->namespaze, klass->name);

            // Avoid managed enumeration here: it can crash if we accidentally operate on a non-managed object.
            // We only support System.Collections.Generic.List`1 layout (items/size/version/syncRoot).
            int idx = 0;
            auto hdrKlass = Il2cppUtils::get_class_from_instance(headersObj);
            if (!hdrKlass || !hdrKlass->namespaze || !hdrKlass->name) {
                Log::WarnFmt("[ApiClient_Deserialize] headers klass invalid headers=%p", headersObj);
                return;
            }
            Log::InfoFmt("[ApiClient_Deserialize] headers runtime type=%s.%s", hdrKlass->namespaze, hdrKlass->name);
            const bool isGenericList = std::string_view(hdrKlass->namespaze) == "System.Collections.Generic"
                                    && std::string_view(hdrKlass->name).find("List`1") != std::string_view::npos;
            if (!isGenericList) {
                Log::WarnFmt("[ApiClient_Deserialize] headers type not supported for dump: %s.%s (headers=%p)",
                             hdrKlass->namespaze, hdrKlass->name, headersObj);
                return;
            }

            auto list = reinterpret_cast<UnityResolve::UnityType::List<void*>*>(headersObj);
            if (!list) return;
            const int listSize = list->size;
            auto items = list->pList;
            Log::InfoFmt("[ApiClient_Deserialize] headers list size=%d items=%p", listSize, items);
            if (!items) {
                Log::WarnFmt("[ApiClient_Deserialize] headers list items=null headers=%p", headersObj);
                return;
            }

            const int cap = (int)items->max_length;
            const int n = (listSize < cap) ? listSize : cap;

            // Resolve Parameter.get_Name/get_Value via MethodInfo.
            static void* s_paramKlass = nullptr;
            static Il2cppUtils::MethodInfo* s_paramGetNameMi = nullptr;
            static Il2cppUtils::MethodInfo* s_paramGetValueMi = nullptr;
            if (!s_paramKlass) {
                s_paramKlass = Il2cppUtils::GetClassIl2cpp("RestSharp.dll", "RestSharp", "Parameter");
                if (!s_paramKlass) s_paramKlass = Il2cppUtils::GetClassIl2cpp("RestSharp", "RestSharp", "Parameter");
                if (s_paramKlass) {
                    s_paramGetNameMi = Il2cppUtils::GetMethodIl2cpp(s_paramKlass, "get_Name", 0);
                    s_paramGetValueMi = Il2cppUtils::GetMethodIl2cpp(s_paramKlass, "get_Value", 0);
                }
            }

            for (int i = 0; i < n && idx < 128; ++i) {
                auto param = items->At((unsigned)i);
                if (!param) {
                    ++idx;
                    continue;
                }

                std::string name = "(unknown)";
                std::string value = "(null)";

                if (s_paramGetNameMi) {
                    using GetStrFn = Il2cppUtils::Il2CppString*(*)(void*, Il2cppUtils::MethodInfo*);
                    auto s = reinterpret_cast<GetStrFn>(s_paramGetNameMi->methodPointer)(param, s_paramGetNameMi);
                    if (s) name = s->ToString();
                }

                if (s_paramGetValueMi) {
                    using GetObjFn2 = void*(*)(void*, Il2cppUtils::MethodInfo*);
                    auto v = reinterpret_cast<GetObjFn2>(s_paramGetValueMi->methodPointer)(param, s_paramGetValueMi);
                    if (v) {
                        const auto vk = Il2cppUtils::get_class_from_instance(v);
                        const bool isString = vk
                            && vk->namespaze && std::string_view(vk->namespaze) == "System"
                            && vk->name && std::string_view(vk->name) == "String";
                        if (isString) {
                            value = static_cast<Il2cppUtils::Il2CppString*>(v)->ToString();
                        } else if (vk && vk->namespaze && vk->name) {
                            value = std::string(vk->namespaze) + "." + vk->name;
                        } else {
                            value = "(object)";
                        }
                    }
                }

                if (idx < 40
                    || name == "launcher_info"
                    || name == "fan_level"
                    || name == "chapter_rank_id"
                    || name == "chapter_total_point"
                    || name == "user_stamina"
                    || name == "x-res-version"
                    || name == "x-server-date") {
                    Log::InfoFmt("[ApiClient_Deserialize] header[%d] %s: %s", idx, name.c_str(), value.c_str());
                }
                ++idx;
            }

            // Log::VerboseFmt("[ApiClient_Deserialize] response headers dump end response=%p count=%d", response, idx);
        }

        static nlohmann::json ReadRestResponseHeaders(void* response) {
            nlohmann::json headers = nlohmann::json::array();
            if (!IsRestSharpResponse(response)) return headers;

            auto getHeadersMi = ResolveRestResponseGetHeaders();
            if (!getHeadersMi) return headers;

            UnityResolve::ThreadAttach();

            using GetHeadersFn = void*(*)(void*, Il2cppUtils::MethodInfo*);
            auto headersObj = reinterpret_cast<GetHeadersFn>(getHeadersMi->methodPointer)(response, getHeadersMi);
            if (!headersObj) return headers;

            auto hdrKlass = Il2cppUtils::get_class_from_instance(headersObj);
            const bool isGenericList = hdrKlass
                                    && hdrKlass->namespaze
                                    && hdrKlass->name
                                    && std::string_view(hdrKlass->namespaze) == "System.Collections.Generic"
                                    && std::string_view(hdrKlass->name).find("List`1") != std::string_view::npos;
            if (!isGenericList) return headers;

            auto list = reinterpret_cast<UnityResolve::UnityType::List<void*>*>(headersObj);
            if (!list || !list->pList) return headers;

            static void* s_paramKlass = nullptr;
            static Il2cppUtils::MethodInfo* s_paramGetNameMi = nullptr;
            static Il2cppUtils::MethodInfo* s_paramGetValueMi = nullptr;
            static bool s_paramResolved = false;
            if (!s_paramResolved) {
                s_paramResolved = true;
                s_paramKlass = Il2cppUtils::GetClassIl2cpp("RestSharp.dll", "RestSharp", "Parameter");
                if (!s_paramKlass) s_paramKlass = Il2cppUtils::GetClassIl2cpp("RestSharp", "RestSharp", "Parameter");
                if (s_paramKlass) {
                    s_paramGetNameMi = Il2cppUtils::GetMethodIl2cpp(s_paramKlass, "get_Name", 0);
                    s_paramGetValueMi = Il2cppUtils::GetMethodIl2cpp(s_paramKlass, "get_Value", 0);
                }
            }

            const int cap = static_cast<int>(list->pList->max_length);
            const int n = (list->size < cap) ? list->size : cap;
            for (int i = 0; i < n && i < 256; ++i) {
                auto param = list->pList->At(static_cast<unsigned>(i));
                if (!param) continue;

                std::string name;
                std::string value;

                if (s_paramGetNameMi) {
                    using GetStrFn = Il2cppUtils::Il2CppString*(*)(void*, Il2cppUtils::MethodInfo*);
                    auto s = reinterpret_cast<GetStrFn>(s_paramGetNameMi->methodPointer)(param, s_paramGetNameMi);
                    if (s) name = s->ToString();
                }

                if (s_paramGetValueMi) {
                    using GetObjFn = void*(*)(void*, Il2cppUtils::MethodInfo*);
                    auto v = reinterpret_cast<GetObjFn>(s_paramGetValueMi->methodPointer)(param, s_paramGetValueMi);
                    if (v) {
                        const auto vk = Il2cppUtils::get_class_from_instance(v);
                        const bool isString = vk
                            && vk->namespaze
                            && std::string_view(vk->namespaze) == "System"
                            && vk->name
                            && std::string_view(vk->name) == "String";
                        if (isString) {
                            value = static_cast<Il2cppUtils::Il2CppString*>(v)->ToString();
                        } else if (vk && vk->namespaze && vk->name) {
                            value = std::string(vk->namespaze) + "." + vk->name;
                        } else {
                            value = "(object)";
                        }
                    }
                }

                headers.push_back({
                    {"name", name},
                    {"value", value},
                });
            }
            return headers;
        }

        static nlohmann::json ReadRestResponseFields(void* response) {
            nlohmann::json out = nlohmann::json::object();
            if (!IsRestSharpResponse(response)) return out;

            UnityResolve::ThreadAttach();

            auto contentMi = ResolveRestResponseGetter("get_Content");
            if (contentMi) {
                using GetContentFn = Il2cppUtils::Il2CppString*(*)(void*, Il2cppUtils::MethodInfo*);
                auto content = reinterpret_cast<GetContentFn>(contentMi->methodPointer)(response, contentMi);
                out["content"] = content ? content->ToString() : "";
            }

            auto statusCodeMi = ResolveRestResponseGetter("get_StatusCode");
            if (statusCodeMi) {
                using GetIntFn = int(*)(void*, Il2cppUtils::MethodInfo*);
                out["status_code"] = reinterpret_cast<GetIntFn>(statusCodeMi->methodPointer)(response, statusCodeMi);
            }

            auto statusDescriptionMi = ResolveRestResponseGetter("get_StatusDescription");
            if (statusDescriptionMi) {
                using GetStringFn = Il2cppUtils::Il2CppString*(*)(void*, Il2cppUtils::MethodInfo*);
                auto desc = reinterpret_cast<GetStringFn>(statusDescriptionMi->methodPointer)(response, statusDescriptionMi);
                out["status_description"] = desc ? desc->ToString() : "";
            }

            auto responseStatusMi = ResolveRestResponseGetter("get_ResponseStatus");
            if (responseStatusMi) {
                using GetIntFn = int(*)(void*, Il2cppUtils::MethodInfo*);
                out["response_status"] = reinterpret_cast<GetIntFn>(responseStatusMi->methodPointer)(response, responseStatusMi);
            }

            out["headers"] = ReadRestResponseHeaders(response);
            return out;
        }

        void RememberOfficialApiRequest(const std::string& path, const nlohmann::json& request) {
            std::lock_guard<std::mutex> lock(apiAuditContextMutex);
            lastOfficialApiPath = path;
            lastOfficialApiRequest = request.dump();
        }

        nlohmann::json CurrentOfficialApiRequestContext() {
            std::lock_guard<std::mutex> lock(apiAuditContextMutex);
            return {
                {"path", lastOfficialApiPath},
                {"request", lastOfficialApiRequest},
            };
        }

        void AppendOfficialApiDump(nlohmann::json event) {
            auto context = CurrentOfficialApiRequestContext();
            event["request_path"] = context.value("path", "");
            event["request"] = context.value("request", "");
            event["current_client_version"] = Config::currentClientVersion.toString();
            event["current_res_version"] = Config::currentResVersion;
            event["api_source"] = Config::enableOfflineApiMock ? "selfhost" : "official";

            std::error_code ec;
            const auto dumpPath = Local::GetBasePath().parent_path() / "official_api_dump.jsonl";
            std::filesystem::create_directories(dumpPath.parent_path(), ec);
            static std::mutex dumpMutex;
            std::lock_guard<std::mutex> lock(dumpMutex);
            std::ofstream ofs(dumpPath, std::ios::app);
            if (ofs.is_open()) {
                ofs << event.dump() << '\n';
            }
        }

        void AddResponseType(nlohmann::json& event, void* type) {
            auto klass = UnityResolve::Invoke<void*>("il2cpp_class_from_system_type", type);
            if (!klass) return;

            auto ns = UnityResolve::Invoke<const char*>("il2cpp_class_get_namespace", klass);
            auto name = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", klass);
            event["response_type"] = std::string(ns ? ns : "") + "." + (name ? name : "");
        }

        void AppendOfficialApiRawResponseDump(void* response, void* type) {
            nlohmann::json event = {
                {"kind", Config::enableOfflineApiMock ? "selfhost_api_rest_response" : "official_api_rest_response"},
                {"rest_response", ReadRestResponseFields(response)},
            };
            AddResponseType(event, type);
            AppendOfficialApiDump(std::move(event));
        }

        void AppendOfficialApiResponseDump(const nlohmann::json& responseJson, void* type) {
            nlohmann::json event = {
                {"kind", Config::enableOfflineApiMock ? "selfhost_api_deserialized_response" : "official_api_deserialized_response"},
                {"response", responseJson},
            };
            AddResponseType(event, type);
            AppendOfficialApiDump(std::move(event));
        }

        nlohmann::json ObjectToJsonOrString(void* value) {
            if (!value) return nullptr;

            const auto klass = Il2cppUtils::get_class_from_instance(value);
            const bool isString = klass
                && klass->namespaze
                && std::string_view(klass->namespaze) == "System"
                && klass->name
                && std::string_view(klass->name) == "String";
            if (isString) {
                return static_cast<Il2cppUtils::Il2CppString*>(value)->ToString();
            }

            auto jsonString = Il2cppUtils::ToJsonStr(value);
            if (!jsonString) return "(serialize_failed)";
            auto json = nlohmann::json::parse(jsonString->ToString(), nullptr, false);
            if (json.is_discarded()) return jsonString->ToString();
            return json;
        }

        int AddLowercaseStringHeaderAliases(void* header, const nlohmann::json& headerJson) {
            if (!header || !headerJson.is_object()) return 0;

            auto klass = Il2cppUtils::get_class_from_instance(header);
            if (!klass) return 0;

            auto containsKeyMethod = Il2cppUtils::GetMethodIl2cpp(klass, "ContainsKey", 1);
            auto addMethod = Il2cppUtils::GetMethodIl2cpp(klass, "Add", 2);
            if (!containsKeyMethod || !addMethod) return 0;

            using ContainsKeyFn = bool(*)(void*, Il2cppUtils::Il2CppString*, Il2cppUtils::MethodInfo*);
            using AddFn = void(*)(void*, Il2cppUtils::Il2CppString*, Il2cppUtils::Il2CppString*, Il2cppUtils::MethodInfo*);
            const auto containsKey = reinterpret_cast<ContainsKeyFn>(containsKeyMethod->methodPointer);
            const auto add = reinterpret_cast<AddFn>(addMethod->methodPointer);

            int added = 0;
            for (const auto& item : headerJson.items()) {
                if (!item.value().is_string()) continue;

                auto loweredKey = LowercaseAscii(item.key());
                if (loweredKey == item.key()) continue;

                auto il2cppKey = Il2cppUtils::Il2CppString::New(loweredKey);
                if (containsKey(header, il2cppKey, containsKeyMethod)) continue;

                auto il2cppValue = Il2cppUtils::Il2CppString::New(item.value().get<std::string>());
                add(header, il2cppKey, il2cppValue, addMethod);
                ++added;
            }
            return added;
        }
    } // namespace

    namespace Shareable {
        // memory leak ?
        std::unordered_map<std::string, ArchiveData> archiveData{};
        void* realtimeRenderingArchiveControllerCache = nullptr;
        float realtimeRenderingArchivePositionSeconds = 0;
        std::string currentArchiveId = "";
        float currentArchiveDuration = 0;
        RenderScene renderScene = RenderScene::None;
        SetPlayPosition_State setPlayPositionState = SetPlayPosition_State::Nothing;

        // Function implementations
        void resetRenderScene() {
            renderScene = RenderScene::None;
        }

        bool renderSceneIsNone() {
            return renderScene == RenderScene::None;
        }

        bool renderSceneIsFesLive() {
            return renderScene == RenderScene::FesLive;
        }

        bool renderSceneIsWithLive() {
            return renderScene == RenderScene::WithLive;
        }

        bool renderSceneIsStory() {
            return renderScene == RenderScene::Story;
        }
    }

    std::string replaceUriHost(const std::string& uri, const std::string& assets_url) {
        std::string pattern = R"(^https://[^/]+(/.*)?$)";
        RE2 re(pattern);
        
        if (!re.ok()) {
            Log::WarnFmt("RE2 compile failed for pattern: %s, error: %s", pattern.c_str(), re.error().c_str());
            return uri;
        }
        
        std::string path_part;
        if (RE2::FullMatch(uri, re, &path_part)) {
            std::string result = assets_url;
            if (!path_part.empty()) {
                if (!assets_url.empty() && assets_url.back() == '/' && path_part.front() == '/') {
                    result += path_part.substr(1);
                } else if (!assets_url.empty() && assets_url.back() != '/' && path_part.front() != '/') {
                    result += "/" + path_part;
                } else {
                    result += path_part;
                }
            }
            Log::VerboseFmt("URL replaced: %s -> %s", uri.c_str(), result.c_str());
            return result;
        }
        Log::VerboseFmt("URI left unchanged: %s", uri.c_str());
        return uri;
    }

    bool IsOfficialAssetUrl(const std::string& uri) {
        return uri.starts_with("https://assets.link-like-lovelive.app/");
    }

    void AppendOfficialRequestAudit(const std::string& kind, const std::string& target, const nlohmann::json& detail) {
        static std::mutex auditMutex;
        nlohmann::json event = {
            {"kind", kind},
            {"target", target},
            {"detail", detail},
            {"current_client_version", Config::currentClientVersion.toString()},
            {"current_res_version", Config::currentResVersion},
        };

        Log::WarnFmt("[OfficialRequestAudit] kind=%s target=%s detail=%s",
                     kind.c_str(), target.c_str(), detail.dump().c_str());

        std::error_code ec;
        const auto auditPath = Local::GetBasePath().parent_path() / "official_request_audit.jsonl";
        std::filesystem::create_directories(auditPath.parent_path(), ec);
        std::lock_guard<std::mutex> lock(auditMutex);
        std::ofstream ofs(auditPath, std::ios::app);
        if (ofs.is_open()) {
            ofs << event.dump() << '\n';
        }
    }

    void AppendOfficialApiExceptionDump(const std::string& exceptionText) {
        AppendOfficialApiDump({
            {"kind", "official_api_exception"},
            {"exception", exceptionText},
        });
    }

    void AppendOfficialApiExceptionCtorDump(int errorCode,
                                            Il2cppUtils::Il2CppString* message,
                                            void* errorContent,
                                            void* headers) {
        nlohmann::json event = {
            {"kind", "official_api_exception_ctor"},
            {"error_code", errorCode},
            {"message", message ? message->ToString() : ""},
        };
        if (errorContent) {
            event["error_content"] = ObjectToJsonOrString(errorContent);
        }
        if (headers) {
            event["headers"] = ObjectToJsonOrString(headers);
        }
        AppendOfficialApiDump(std::move(event));
    }

    bool RewriteOfficialAssetUrls(nlohmann::json& value) {
        bool rewritten = false;
        if (value.is_object()) {
            for (auto& item : value.items()) {
                rewritten |= RewriteOfficialAssetUrls(item.value());
            }
            return rewritten;
        }
        if (value.is_array()) {
            for (auto& item : value) {
                rewritten |= RewriteOfficialAssetUrls(item);
            }
            return rewritten;
        }
        if (!value.is_string() || Config::assetsUrlPrefix.empty()) {
            return false;
        }

        const auto uri = value.get<std::string>();
        if (!IsOfficialAssetUrl(uri)) {
            return false;
        }
        const auto rewrittenUri = replaceUriHost(uri, Config::assetsUrlPrefix);
        AppendOfficialRequestAudit("asset_url_rewritten", uri, {{"rewritten", rewrittenUri}});
        value = rewrittenUri;
        return true;
    }

    bool isMotionCaptureCompatible(const std::string & url, const nlohmann::json& archive_config) {
        bool isCompatible = true;
        auto isAlsArchive = url.ends_with("md");
        isCompatible &= Config::isLegacyMrsVersion() ^ isAlsArchive;
        if (archive_config.contains("version_compatibility") && 
            !archive_config["version_compatibility"].is_null() && 
            archive_config["version_compatibility"].is_object()) {
            auto version_compatibility = archive_config["version_compatibility"];
//            Log::DebugFmt("version_compatibility is %s", version_compatibility.dump().c_str());
            if (version_compatibility.contains("rule") && !version_compatibility["rule"].is_null()) {
                std::string rule = version_compatibility["rule"].get<std::string>();
                isCompatible &= VersionCompatibility::VersionChecker(rule).checkCompatibility(Config::currentClientVersion);
            }
        }
        return isCompatible;
    }
#pragma region HttpRequests
    nlohmann::json handle_legacy_archive_data(nlohmann::json json) {
        json["live_timeline_ids"] = json["timeline_ids"];
        nlohmann::json character_ids = nlohmann::json::array();
        for (const auto& character : json["characters"]) {
            character_ids.push_back(character["character_id"]);
        }
        json["character_ids"] = character_ids;
//        json["_id"] = 23;
//        json["costume_ids"] = {3016};
        Log::VerboseFmt("%s", json.dump().c_str());
        if (Config::isLegacyMrsVersion()) {
            for (auto& chapter : json["chapters"]) {
                chapter["is_available"] = "true";
            }
        }
        return json;
    }

    void clear_json_arr(nlohmann::json& json, const std::string& key) {
        if (json.contains(key) && json[key].is_array()) {
            json[key].clear();
        }
    }
    nlohmann::json handle_get_with_archive_data(nlohmann::json json, bool is_legacy = false) {
        if (Config::withliveOrientation == (int)HookLiveRender::LiveScreenOrientation::Landscape) {
            json["is_horizontal"] = "true";
        }
        if (Config::withliveOrientation == (int)HookLiveRender::LiveScreenOrientation::Portrait) {
            json["is_horizontal"] = "false";
        }
        if (Config::unlockAfter) {
            json["has_extra_admission"] = "true";
        }
        if (Config::enableSetArchiveStartTime) {
            json["chapters"][0]["play_time_second"] = Config::archiveStartTime;
        }
        if (Config::enableMotionCaptureReplay) {
            auto archive_id = Shareable::currentArchiveId;
            auto it = Config::archiveConfigMap.find(archive_id);
            if (it == Config::archiveConfigMap.end()) return json;
            auto archive_config = it->second;
            auto replay_type = archive_config["replay_type"].get<uint>();
            auto external_link = archive_config.contains("external_link") ? archive_config["external_link"].get<std::string>() : "";
            auto external_fix_link = archive_config.contains("external_fix_link") ? archive_config["external_fix_link"].get<std::string>() : "";

            auto assets_url = Config::motionCaptureResourceUrl;
            if (replay_type == 0) {
                json.erase("archive_url");
            }
            if (replay_type == 1) {
                json.erase("video_url");
                if (!external_link.empty()) {
                    auto new_external_link = replaceUriHost(external_link, assets_url);
                    json["archive_url"] = new_external_link;
                }
            }
            if (replay_type == 2) {
                json.erase("video_url");
                if (!external_fix_link.empty()) {
                    auto new_external_fix_link = replaceUriHost(external_fix_link, assets_url);
                    json["archive_url"] = new_external_fix_link;
                }
            }
            clear_json_arr(json, "timelines");
            clear_json_arr(json, "gift_pt_rankings");
        }
        if (is_legacy) {
            json = handle_legacy_archive_data(json);
        }
        return json;
    }
    nlohmann::json handle_get_fes_archive_data(nlohmann::json json, bool is_legacy = false) {
        if (Config::unlockAfter) {
            json["has_extra_admission"] = "true";
        }
        if (Config::fesArchiveUnlockTicket) {
            json["selectable_camera_types"] = {1,2,3,4};
            json["ticket_rank"] = 6;
        }
        if (Config::enableSetArchiveStartTime) {
            json["chapters"][0]["play_time_second"] = Config::archiveStartTime;
        }
        if (Config::enableMotionCaptureReplay) {
            auto archive_id = Shareable::currentArchiveId;
            auto it = Config::archiveConfigMap.find(archive_id);
            if (it == Config::archiveConfigMap.end()) return json;
            auto archive_config = it->second;
            auto replay_type = archive_config["replay_type"].get<uint>();
            auto external_link = archive_config.contains("external_link") ? archive_config["external_link"].get<std::string>() : "";
            auto external_fix_link = archive_config.contains("external_fix_link") ? archive_config["external_fix_link"].get<std::string>() : "";
            auto assets_url = Config::motionCaptureResourceUrl;
            if (replay_type == 0) {
                json.erase("archive_url");
            }
            if (replay_type == 1) {
                json.erase("video_url");
                if (!external_link.empty()) {
                    auto new_external_link = replaceUriHost(external_link, assets_url);
                    json["archive_url"] = new_external_link;
                }
            }
            if (replay_type == 2) {
                json.erase("video_url");
                if (!external_fix_link.empty()) {
                    auto new_external_fix_link = replaceUriHost(external_fix_link, assets_url);
                    json["archive_url"] = new_external_fix_link;
                }
            }
            clear_json_arr(json, "timelines");
            clear_json_arr(json, "gift_pt_rankings");
        }
        if (is_legacy) {
            json = handle_legacy_archive_data(json);
        }
        return json;
    }
    bool filter_archive_by_rule(nlohmann::json archive) {
        if (!Config::enableMotionCaptureReplay || !Config::filterMotionCaptureReplay) return false;
        auto archive_id = archive["archives_id"].get<std::string>();
        auto it = Config::archiveConfigMap.find(archive_id);
        if (it == Config::archiveConfigMap.end()) {
            return true; // not found should be filtered
        }
        auto archive_config = it->second;

        /**
         * filter by simple replay type
         */
        auto replay_type = archive_config["replay_type"].get<uint>();
        if (replay_type == 0) return true;

        // apply rule

        // judge motion capture version is compatible with current client
        if (Config::filterPlayableMotionCapture) {
            if (replay_type == 1) {
                if (!isMotionCaptureCompatible(archive_config["external_link"].get<std::string>(), archive_config)) {
                    return true;
                }
            }
            if (replay_type == 2) {
                if (!isMotionCaptureCompatible(archive_config["external_fix_link"].get<std::string>(), archive_config)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool try_handle_get_archive_data(const std::string& archive_id) {
        std::string message = "The motion replay is not compatible for current client!";
        bool avoid_next = false;
        if (Config::enableMotionCaptureReplay && Config::avoidAccidentalTouch) {
            auto it = Config::archiveConfigMap.find(archive_id);
            if (it == Config::archiveConfigMap.end()) return false;
            auto archive_config = it->second;

            if (archive_config.contains("version_compatibility") &&
                  !archive_config["version_compatibility"].is_null() &&
                  archive_config["version_compatibility"].is_object()) {
                auto version_compatibility = archive_config["version_compatibility"];
                if (version_compatibility.contains("message") && !version_compatibility["message"].is_null()) {
                    message = version_compatibility["message"].get<std::string>();
                }
            }
            // client version is valid
            auto replay_type = archive_config["replay_type"].get<uint>();
            if (replay_type == 0) {
                return false;
            }
            if (replay_type == 1) {
                if (!isMotionCaptureCompatible(archive_config["external_link"].get<std::string>(), archive_config)) {
                    avoid_next = true;
                }
            }
            if (replay_type == 2) {
                if (!isMotionCaptureCompatible(archive_config["external_fix_link"].get<std::string>(), archive_config)) {
                    avoid_next = true;
                }
            }
        }
        if (avoid_next) {
            Log::ShowToast(message.c_str());
        }
        return avoid_next;
    }

    nlohmann::json handle_get_archive_list(nlohmann::json json, bool is_legacy = false) {
        auto& archive_list = json["archive_list"];
        archive_list.erase(
                std::remove_if(archive_list.begin(), archive_list.end(),
                               [](const nlohmann::json& archive) {
                                   return filter_archive_by_rule(archive);
                }),
                archive_list.end());
        for (auto& archive : archive_list) {
            auto archive_id = archive["archives_id"].get<std::string>();
            if (Config::unlockAfter) {
                archive["has_extra_admission"] = "true";
            }
            if (Shareable::archiveData.find(archive_id) == Shareable::archiveData.end()) {
                auto live_start_time = archive["live_start_time"].get<std::string>();
                auto live_end_time = archive["live_end_time"].get<std::string>();
                auto duration = LinkuraLocal::Misc::Time::parseISOTime(live_end_time) - LinkuraLocal::Misc::Time::parseISOTime(live_start_time);
                Shareable::archiveData[archive_id] = {
                        .id = archive_id,
                        .duration = duration,
                };
                Log::VerboseFmt("archives id is %s, duration is %lld", archive_id.c_str(), duration);
            }
            if (Config::enableMotionCaptureReplay && Config::enableInGameReplayDisplay) {
                auto it = Config::archiveConfigMap.find(archive_id);
                if (it == Config::archiveConfigMap.end()) continue;
                auto archive_config = it->second;
                auto replay_type = archive_config["replay_type"].get<uint>();
                auto archive_title = archive["name"].get<std::string>();
                std::string recommendVersion;
                if (archive_config.contains("version_compatibility") &&
                    !archive_config["version_compatibility"].is_null() &&
                    archive_config["version_compatibility"].is_object()) {
                    auto version_compatibility = archive_config["version_compatibility"];
                    if (version_compatibility.contains("rule") &&
                        !version_compatibility["rule"].is_null()) {
                        std::string rule = version_compatibility["rule"].get<std::string>();
                        recommendVersion = Config::getRecommendVersion(rule);
                    }
                }
                recommendVersion = recommendVersion.empty() ? recommendVersion : "[" + recommendVersion + "]";

                /**
                 * isMrsVersion isAlsArchive Playable
                 * 0            0            0
                 * 0            1            1
                 * 1            0            1
                 * 1            1            0
                 *
                 * Exclusive or: isMrsVersion ^ isAls
                 */
                if (replay_type == 1) { // motion capture replay
                    std::string mark = isMotionCaptureCompatible(archive_config["external_link"].get<std::string>(), archive_config) ? "✅" : "❌" + recommendVersion;
                    archive_title = mark + archive_title;
                }
                if (replay_type == 2) {
                    std::string mark = isMotionCaptureCompatible(archive_config["external_fix_link"].get<std::string>(), archive_config) ? "☑️" : "❌" + recommendVersion;
                    archive_title = mark + archive_title;
                }
                if (replay_type == 0) { // video replay
                    archive_title = "📺" + archive_title;
                }
                archive["name"] = archive_title;
            }
            if (is_legacy) {
                if (archive["live_type"].get<int>() == 2) { // with meets
                    archive["ticket_rank"] = 1;
                }
            }
        }
        if (is_legacy) {
            json.erase("filterable_characters");
            json.erase("sortable_fields");
        }
        return json;
    }

    void* advSeriesMasterInstance = nullptr;
    std::unordered_set<int64_t> advSeriesIdExistsCache;
    std::unordered_set<int64_t> advSeriesIdNotExistsCache;

    void* advDatasMasterInstance = nullptr;
    std::unordered_set<int64_t> advDataIdExistsCache;
    std::unordered_set<int64_t> advDataIdNotExistsCache;

    DEFINE_HOOK(void, Silverflame_SFL_AdvSeriesMaster_ctor, (void* self, void* conn)) {
        Log::DebugFmt("Silverflame_SFL_AdvSeriesMaster_ctor HOOKED, instance=%p", self);
        advSeriesMasterInstance = self;
        // 清空缓存
        advSeriesIdExistsCache.clear();
        advSeriesIdNotExistsCache.clear();
        return Silverflame_SFL_AdvSeriesMaster_ctor_Orig(self, conn);
    }

    DEFINE_HOOK(void*, Silverflame_SFL_AdvSeriesMaster_Fetch, (void* self, int64_t id)) {
        void* result = Silverflame_SFL_AdvSeriesMaster_Fetch_Orig(self, id);
        return result;
    }

    DEFINE_HOOK(void, Silverflame_SFL_AdvDatasMaster_ctor, (void* self, void* conn)) {
        Log::DebugFmt("Silverflame_SFL_AdvDatasMaster_ctor HOOKED, instance=%p", self);
        advDatasMasterInstance = self;
        // 清空缓存
        advDataIdExistsCache.clear();
        advDataIdNotExistsCache.clear();
        return Silverflame_SFL_AdvDatasMaster_ctor_Orig(self, conn);
    }

    DEFINE_HOOK(void*, Silverflame_SFL_AdvDatasMaster_Fetch, (void* self, int64_t id)) {
        void* result = Silverflame_SFL_AdvDatasMaster_Fetch_Orig(self, id);
        return result;
    }

    // 辅助函数：检查 adv_series_id 是否存在于本地数据库（带缓存）
    bool isAdvSeriesIdExists(int64_t id) {
        if (advSeriesIdExistsCache.count(id)) {
            return true;
        }
        if (advSeriesIdNotExistsCache.count(id)) {
            return false;
        }

        if (!advSeriesMasterInstance) {
            Log::WarnFmt("AdvSeriesMaster instance is null, cannot check id=%lld", id);
            return true; // 如果实例不存在，默认返回 true 不过滤
        }
        void* result = Silverflame_SFL_AdvSeriesMaster_Fetch_Orig(advSeriesMasterInstance, id);
        bool exists = result != nullptr;

        if (exists) {
            advSeriesIdExistsCache.insert(id);
        } else {
            advSeriesIdNotExistsCache.insert(id);
            Log::DebugFmt("adv_series_id=%lld not found in local database", id);
        }

        return exists;
    }

    bool isAdvDataIdExists(int64_t id) {
        if (advDataIdExistsCache.count(id)) {
            return true;
        }
        if (advDataIdNotExistsCache.count(id)) {
            return false;
        }

        if (!advDatasMasterInstance) {
            Log::WarnFmt("AdvDatasMaster instance is null, cannot check id=%lld", id);
            return true; // 如果实例不存在，默认返回 true 不过滤
        }
        void* result = Silverflame_SFL_AdvDatasMaster_Fetch_Orig(advDatasMasterInstance, id);
        bool exists = result != nullptr;

        if (exists) {
            advDataIdExistsCache.insert(id);
        } else {
            advDataIdNotExistsCache.insert(id);
            Log::DebugFmt("adv_data_id=%lld not found in local database", id);
        }

        return exists;
    }

    template<typename Predicate>
    void json_erase_if(nlohmann::json& arr, Predicate pred) {
        arr.erase(std::remove_if(arr.begin(), arr.end(), pred), arr.end());
    }

    void filterActivityRecordMonthlyInfoList(nlohmann::json& json) {
        if (!json.contains("activity_record_monthly_info_list") || !json["activity_record_monthly_info_list"].is_array()) {
            return;
        }
        auto& info_list = json["activity_record_monthly_info_list"];

        json_erase_if(info_list, [](const nlohmann::json& info_item) {
            auto adv_series_id = info_item.value("adv_series_id", 0LL);
            if (adv_series_id && !isAdvSeriesIdExists(adv_series_id)) {
                Log::DebugFmt("Filtering out adv_series_id=%lld", adv_series_id);
                return true;
            }
            return false;
        });

        for (auto& info_item : info_list) {
            if (!info_item.contains("adv_info_list") || !info_item["adv_info_list"].is_array()) continue;
            json_erase_if(info_item["adv_info_list"], [](const nlohmann::json& adv_info) {
                auto adv_data_id = adv_info.value("adv_data_id", 0LL);
                if (adv_data_id && !isAdvDataIdExists(adv_data_id)) {
                    Log::DebugFmt("Filtering out adv_data_id=%lld", adv_data_id);
                    return true;
                }
                return false;
            });
        }
    }

    uintptr_t ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync_MoveNext_Addr = 0;
    uintptr_t ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync_MoveNext_Addr = 0;
    /**
     * Legacy version 1.x.x
     */
    uintptr_t ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync_Old_MoveNext_Addr = 0;
    uintptr_t ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync_Old_MoveNext_Addr = 0;
    uintptr_t ArchiveApi_ArchiveGetArchiveList_Old_MoveNext_Addr = 0;

    uintptr_t ArchiveApi_ArchiveGetArchiveList_MoveNext_Addr = 0;
    uintptr_t WithliveApi_WithliveEnterWithHttpInfoAsync_MoveNext_Addr = 0;
    uintptr_t FesliveApi_FesliveEnterWithHttpInfoAsync_MoveNext_Addr = 0;

    uintptr_t WebviewLiveApi_WebviewLiveLiveInfoWithHttpInfoAsync_MoveNext_Addr = 0;
    uintptr_t WithliveApi_WithliveLiveInfoWithHttpInfoAsync_MoveNext_Addr = 0;
    uintptr_t FesliveApi_FesliveLiveInfoWithHttpInfoAsync_MoveNext_Addr = 0;
    uintptr_t ArchiveApi_ArchiveWithliveInfoWithHttpInfoAsync_MoveNext_Addr = 0;

    uintptr_t ActivityRecordGetTopWithHttpInfoAsync_MoveNext_Addr = 0;

    // http request log
        static std::string GetSelfhostApiBaseUrl() {
            if (!Config::apiMockBaseUrl.empty()) return Config::apiMockBaseUrl;
            return "";
        }

        static std::string ExtractUrlPathAndQuery(const std::string& url) {
            const auto schemePos = url.find("://");
            if (schemePos == std::string::npos) {
                return url.starts_with("/") ? url : "";
            }
            const auto pathPos = url.find('/', schemePos + 3);
            if (pathPos == std::string::npos) {
                return "/";
            }
            return url.substr(pathPos);
        }

        static std::string JoinBaseUrlAndPath(const std::string& baseUrl, const std::string& pathAndQuery) {
            if (baseUrl.empty()) return pathAndQuery;
            if (pathAndQuery.empty()) return baseUrl;
            if (baseUrl.back() == '/' && pathAndQuery.front() == '/') {
                return baseUrl + pathAndQuery.substr(1);
            }
            if (baseUrl.back() != '/' && pathAndQuery.front() != '/') {
                return baseUrl + "/" + pathAndQuery;
            }
            return baseUrl + pathAndQuery;
        }

        static bool IsOfficialApiOrWebUrl(const std::string& url) {
            return url.starts_with("https://api.link-like-lovelive.app/")
                || url.starts_with("https://link-like-lovelive.app/");
        }

        static bool IsOfficialTopWebUrl(const std::string& url) {
            return url.starts_with("https://link-like-lovelive.app/");
        }

        static nlohmann::json CreateWebViewRewriteState(const std::string& original, const char* source, bool hasValue) {
            const auto pathAndQuery = ExtractUrlPathAndQuery(original);
            return {
                {"source", source},
                {"has_value", hasValue},
                {"enable_offline_api_mock", Config::enableOfflineApiMock},
                {"api_mock_base_url", Config::apiMockBaseUrl},
                {"top_url_prefix", Config::topUrlPrefix},
                {"assets_url_prefix", Config::assetsUrlPrefix},
                {"is_official_asset_url", IsOfficialAssetUrl(original)},
                {"is_official_top_web_url", IsOfficialTopWebUrl(original)},
                {"is_official_api_or_web_url", IsOfficialApiOrWebUrl(original)},
                {"starts_with_slash", original.starts_with("/")},
                {"path_and_query", pathAndQuery}
            };
        }

        static Il2cppUtils::Il2CppString* RewriteWebViewUrlString(Il2cppUtils::Il2CppString* value, const char* source) {
            const auto original = value ? value->ToString() : "(null)";
            AppendOfficialRequestAudit("webview_url_opened", original, {{"source", source}});
            AppendOfficialRequestAudit("webview_rewrite_state", original, CreateWebViewRewriteState(original, source, value != nullptr));

            if (!value) {
                AppendOfficialRequestAudit("webview_url_not_rewritten", original, {{"source", source}, {"reason", "null_value"}});
                return value;
            }

            if (!Config::enableOfflineApiMock) {
                AppendOfficialRequestAudit("webview_url_not_rewritten", original, {{"source", source}, {"reason", "offline_api_mock_disabled"}});
                return value;
            }

            if (IsOfficialAssetUrl(original)) {
                if (Config::assetsUrlPrefix.empty()) {
                    AppendOfficialRequestAudit("selfhost_asset_base_url_empty", original, {{"source", source}});
                    return Il2cppUtils::Il2CppString::New("about:blank");
                }
                const auto rewritten = replaceUriHost(original, Config::assetsUrlPrefix);
                AppendOfficialRequestAudit("webview_url_rewritten", original, {{"rewritten", rewritten}, {"source", source}});
                return Il2cppUtils::Il2CppString::New(rewritten);
            }

            if (IsOfficialTopWebUrl(original)) {
                if (Config::topUrlPrefix.empty()) {
                    AppendOfficialRequestAudit("selfhost_top_url_empty", original, {{"source", source}});
                    return Il2cppUtils::Il2CppString::New("about:blank");
                }
                const auto rewritten = JoinBaseUrlAndPath(Config::topUrlPrefix, ExtractUrlPathAndQuery(original));
                AppendOfficialRequestAudit("webview_url_rewritten", original, {{"rewritten", rewritten}, {"source", source}});
                return Il2cppUtils::Il2CppString::New(rewritten);
            }

            if (!IsOfficialApiOrWebUrl(original) && !original.starts_with("/")) {
                AppendOfficialRequestAudit("webview_url_not_rewritten", original, {{"source", source}, {"reason", "non_official_url"}});
                return value;
            }

            const auto selfhostApiBaseUrl = GetSelfhostApiBaseUrl();
            if (selfhostApiBaseUrl.empty()) {
                AppendOfficialRequestAudit("selfhost_api_base_url_empty", original, {{"source", source}});
                return Il2cppUtils::Il2CppString::New("about:blank");
            }

            const auto pathAndQuery = ExtractUrlPathAndQuery(original);
            if (pathAndQuery.empty()) {
                AppendOfficialRequestAudit("webview_url_not_rewritten", original, {{"source", source}, {"reason", "empty_path_and_query"}});
                return value;
            }

            const auto rewritten = JoinBaseUrlAndPath(selfhostApiBaseUrl, pathAndQuery);
            AppendOfficialRequestAudit("webview_url_rewritten", original, {{"rewritten", rewritten}, {"source", source}});
            return Il2cppUtils::Il2CppString::New(rewritten);
        }

        static Il2cppUtils::Il2CppString* RewriteApiBasePathString(Il2cppUtils::Il2CppString* value, const char* source) {
            if (!Config::enableOfflineApiMock) return value;

            const auto baseUrl = GetSelfhostApiBaseUrl();
            const auto original = value ? value->ToString() : "(null)";
            if (baseUrl.empty()) {
                AppendOfficialRequestAudit("selfhost_api_base_url_empty", original, {{"source", source}});
                Log::ErrorFmt("[SelfhostAudit] selfhost_api_base_url_empty source=%s original=%s",
                              source,
                              original.c_str());
                return Il2cppUtils::Il2CppString::New("http://127.0.0.1:9");
            }

            AppendOfficialRequestAudit("api_basepath_rewritten", original, {{"rewritten", baseUrl}, {"source", source}});
            Log::WarnFmt("[SelfhostAudit] API base path replaced source=%s %s -> %s",
                         source,
                         original.c_str(),
                         baseUrl.c_str());
            return Il2cppUtils::Il2CppString::New(baseUrl);
        }

    DEFINE_HOOK(void*, ApiClient_CallApiAsync, (void* self,
            Il2cppUtils::Il2CppString* path, void* method,
            void* queryParams, void* postBody,
            void* headerParams, void* formParams, void* fileParams, void* pathParams,
            Il2cppUtils::Il2CppString* contentType, void* cancellationToken, void* method_info)) {
        auto strPath = path ? path->ToString() : "(null)";
        std::string strBody = "{}";
        if (postBody) {
            const auto klass = Il2cppUtils::get_class_from_instance(postBody);
            const bool isString = klass
                && klass->name && std::string_view(klass->name) == "String"
                && klass->namespaze && std::string_view(klass->namespaze) == "System";
            if (isString) {
                auto bodyStr = static_cast<Il2cppUtils::Il2CppString*>(postBody);
                strBody = bodyStr->ToString();
            }
        }
        nlohmann::json requestAudit = {
            {"request", strBody},
        };
        Log::VerboseFmt("[ApiClient_CallApiAsync] path: %s\nrequest: %s", strPath.c_str(), strBody.c_str());

        if (Config::enableOfflineApiMock && path) {
            const auto selfhostApiBaseUrl = GetSelfhostApiBaseUrl();
            if (selfhostApiBaseUrl.empty()) {
                AppendOfficialRequestAudit("selfhost_api_base_url_empty", strPath, requestAudit);
                Log::ErrorFmt("[SelfhostAudit] selfhost_api_base_url_empty path=%s", strPath.c_str());
                return nullptr;
            }
            auto selfhostAudit = requestAudit;
            selfhostAudit["base_url"] = selfhostApiBaseUrl;
            AppendOfficialRequestAudit("selfhost_api_request", strPath, selfhostAudit);
            RememberOfficialApiRequest(strPath, requestAudit);
            Log::WarnFmt("[SelfhostAudit] selfhost_api_request path=%s base=%s",
                         strPath.c_str(), selfhostApiBaseUrl.c_str());
            return ApiClient_CallApiAsync_Orig(self, path, method, queryParams, postBody,
                                              headerParams, formParams, fileParams, pathParams,
                                              contentType, cancellationToken, method_info);
        }

        AppendOfficialRequestAudit("official_api_request_allowed", strPath, requestAudit);
        RememberOfficialApiRequest(strPath, requestAudit);
        Log::ErrorFmt("[SelfhostAudit] official_api_request_allowed path=%s request=%s",
                      strPath.c_str(), strBody.c_str());

        return ApiClient_CallApiAsync_Orig(self, path, method, queryParams, postBody,
                                          headerParams, formParams, fileParams, pathParams,
                                          contentType, cancellationToken, method_info);
    }
    // http response modify
    DEFINE_HOOK(void* , ApiClient_Deserialize, (void* self, void* response, void* type, void* method_info)) {
        if (Config::enableOfflineApiMock) {
            NormalizeRestResponseHeaderNamesIfPossible(response);
        }
        if (Config::dbgMode || Config::enableOfflineApiMock) {
            auto caller = __builtin_return_address(0);
            Log::VerboseFmt("[ApiClient_Deserialize] enter self=%p response=%p type=%p caller=%p", self, response, type, caller);
            DumpRestResponseHeadersIfPossible(response);
        }
        AppendOfficialApiRawResponseDump(response, type);
        auto result = ApiClient_Deserialize_Orig(self, response, type, method_info);
        auto json = nlohmann::json::parse(Il2cppUtils::ToJsonStr(result)->ToString(), nullptr, false);
        if (json.is_discarded()) {
            AppendOfficialApiDump({
                {"kind", Config::enableOfflineApiMock ? "selfhost_api_deserialize_json_parse_failed" : "official_api_deserialize_json_parse_failed"},
            });
            return result;
        }
        AppendOfficialApiResponseDump(json, type);
        if (RewriteOfficialAssetUrls(json)) {
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
        // Print API response type name and response body for debugging
        {
            auto klass = UnityResolve::Invoke<void*>("il2cpp_class_from_system_type", type);
            if (klass) {
                auto ns   = UnityResolve::Invoke<const char*>("il2cpp_class_get_namespace", klass);
                auto name = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", klass);
                Log::VerboseFmt("[ApiClient_Deserialize] type: %s.%s\nresponse: %s",
                    ns ? ns : "", name ? name : "", json.dump().c_str());
            }
        }
        auto caller = __builtin_return_address(0);
        IF_CALLER_WITHIN(ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync_MoveNext_Addr, caller, 3000) { // hook /v1/archive/get_fes_archive_data response
            json = handle_get_fes_archive_data(json);
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
        IF_CALLER_WITHIN(ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync_MoveNext_Addr, caller, 3000) { // hook /v1/archive/get_with_archive_data response
            json = handle_get_with_archive_data(json);
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
        IF_CALLER_WITHIN(ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync_Old_MoveNext_Addr, caller, 3000) { // hook /v1/archive/get_fes_archive_data response 1.x.x
            json = handle_get_fes_archive_data(json, Config::isFirstYearVersion());
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
        IF_CALLER_WITHIN(ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync_Old_MoveNext_Addr, caller, 3000) { // hook /v1/archive/get_with_archive_data response 1.x.x
            json = handle_get_with_archive_data(json, Config::isFirstYearVersion());
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
        IF_CALLER_WITHIN(ArchiveApi_ArchiveGetArchiveList_MoveNext_Addr, caller, 3000) { // hook /v1/archive/get_archive_list response
            json = handle_get_archive_list(json);
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
        IF_CALLER_WITHIN(ArchiveApi_ArchiveGetArchiveList_Old_MoveNext_Addr, caller, 3000) { // 1.x.x
            json = handle_get_archive_list(json, Config::isFirstYearVersion());
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
        IF_CALLER_WITHIN(WithliveApi_WithliveEnterWithHttpInfoAsync_MoveNext_Addr, caller, 3000) {
            if (Config::withliveOrientation == (int)HookLiveRender::LiveScreenOrientation::Landscape) {
                json["is_horizontal"] = "true";
            }
            if (Config::withliveOrientation == (int)HookLiveRender::LiveScreenOrientation::Portrait) {
                json["is_horizontal"] = "false";
            }
            if (Config::unlockAfter) {
                json["has_extra_admission"] = "true";
            }
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
//        IF_CALLER_WITHIN(FesliveApi_FesliveEnterWithHttpInfoAsync_MoveNext_Addr, caller, 3000) {
////            if (Config::unlockAfter) {
////                json["has_extra_admission"] = "true";
////            }
//            // if (Config::fesArchiveUnlockTicket) {
//            //     json["selectable_camera_types"] = {1,2,3,4};
//            //     json["ticket_rank"] = 6;
//            // }
//            result = Il2cppUtils::FromJsonStr(json.dump(), type);
//        }
        // live info
        IF_CALLER_WITHIN(ArchiveApi_ArchiveWithliveInfoWithHttpInfoAsync_MoveNext_Addr, caller, 3000) {
            if (Config::unlockAfter) {
                json["has_extra_admission"] = "true";
            }
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
        IF_CALLER_WITHIN(WithliveApi_WithliveLiveInfoWithHttpInfoAsync_MoveNext_Addr, caller, 3000) {
            if (Config::unlockAfter) {
                json["has_extra_admission"] = "true";
//                json["has_admission"] = "true";
            }
            result = Il2cppUtils::FromJsonStr(json.dump(), type);
        }
//        IF_CALLER_WITHIN(FesliveApi_FesliveLiveInfoWithHttpInfoAsync_MoveNext_Addr, caller, 3000) {
////            if (Config::unlockAfter) {
////                json["has_admission"] = "true";
////            }
//            result = Il2cppUtils::FromJsonStr(json.dump(), type);
//        }
        IF_CALLER_WITHIN(ActivityRecordGetTopWithHttpInfoAsync_MoveNext_Addr, caller, 3000) {
            if (Config::enableLegacyCompatibility && !Config::isLatestVersion()) {
                filterActivityRecordMonthlyInfoList(json);
                result = Il2cppUtils::FromJsonStr(json.dump(), type);
            }
        }
        return result;
    }

    DEFINE_HOOK(void* , ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync, (void* self, Il2cppUtils::Il2CppObject* request, void* cancellation_token, void* method_info)) {
        Log::DebugFmt("ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync HOOKED");
        auto json = nlohmann::json::parse(Il2cppUtils::ToJsonStr(request)->ToString());
        auto archive_id = json["archives_id"].get<std::string>();
        if (try_handle_get_archive_data(archive_id)) {
            return nullptr;
        }
        Shareable::currentArchiveId = archive_id;
        Shareable::renderScene = Shareable::RenderScene::FesLive;
        return ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync_Orig(self,
                                                                         request,
                                                                         cancellation_token, method_info);
    }
    DEFINE_HOOK(void* , ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync, (void* self, Il2cppUtils::Il2CppObject* request, void* cancellation_token, void* method_info)) {
        Log::DebugFmt("ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync HOOKED");
        auto json = nlohmann::json::parse(Il2cppUtils::ToJsonStr(request)->ToString());
        auto archive_id = json["archives_id"].get<std::string>();
        if (try_handle_get_archive_data(archive_id)) {
            return nullptr;
        }
        Shareable::currentArchiveId = archive_id;
        Shareable::renderScene = Shareable::RenderScene::WithLive;
        return ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync_Orig(self,
                                                                          request,
                                                                          cancellation_token, method_info);
    }

    DEFINE_HOOK(void*, ArchiveApi_ArchiveGetArchiveListWithHttpInfoAsync, (void* self, Il2cppUtils::Il2CppObject* request, void* cancellation_token, void* method_info)) {
        Log::DebugFmt("ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync HOOKED");
        if (Config::enableMotionCaptureReplay && Config::filterMotionCaptureReplay) {
            auto json = nlohmann::json::parse(Il2cppUtils::ToJsonStr(request)->ToString());
            json.erase("limit");
            json.erase("offset");
            request = static_cast<Il2cppUtils::Il2CppObject*>(
                    Il2cppUtils::FromJsonStr(json.dump(), Il2cppUtils::get_system_type_from_instance(request))
            );
        }
        return ArchiveApi_ArchiveGetArchiveListWithHttpInfoAsync_Orig(self, request, cancellation_token, method_info);
    }

    // cheat for server api, but we need to decrease the abnormal behaviour here. ( camera_type should change when every request sends )
    DEFINE_HOOK(void* ,ArchiveApi_ArchiveSetFesCameraWithHttpInfoAsync, (void* self, Il2cppUtils::Il2CppObject* request, void* cancellation_token, void* method_info)) {
        if (Config::fesArchiveUnlockTicket) {
            return nullptr;
//            auto json = nlohmann::json::parse(Il2cppUtils::ToJsonStr(request)->ToString());
//            json["camera_type"] = 1;
//            if (json.contains("focus_character_id")){
//                json.erase("focus_character_id");
//            }
//            request = static_cast<Il2cppUtils::Il2CppObject*>(
//                    Il2cppUtils::FromJsonStr(json.dump(), Il2cppUtils::get_system_type_from_instance(request))
//            );
        }
        return ArchiveApi_ArchiveSetFesCameraWithHttpInfoAsync_Orig(self,
                                                                    request,
                                                                    cancellation_token, method_info);
    }

    DEFINE_HOOK(void*, ArchiveApi_ArchiveSetFesCameraAsync, (void* self, Il2cppUtils::Il2CppObject* request, void* cancellation_token, void* method_info)) {
        Log::DebugFmt("ArchiveApi_ArchiveSetFesCameraAsync HOOKED");
        return nullptr;
    }
    DEFINE_HOOK(void*, FesliveApi_FesliveSetCameraWithHttpInfoAsync, (void* self, Il2cppUtils::Il2CppObject* request, void* cancellation_token, void* method_info)) {
         if (Config::fesArchiveUnlockTicket) {
             return nullptr;
         }
        return FesliveApi_FesliveSetCameraWithHttpInfoAsync_Orig(self,
                                                                    request,
                                                                    cancellation_token, method_info);
    }

    DEFINE_HOOK(void* , FesliveApi_FesliveEnterWithHttpInfoAsync, (void* self, Il2cppUtils::Il2CppObject* request, void* cancellation_token, void* method_info)) {
        Shareable::renderScene = Shareable::RenderScene::FesLive;
        return FesliveApi_FesliveEnterWithHttpInfoAsync_Orig(self,
                                                                          request,
                                                                          cancellation_token, method_info);
    }

    DEFINE_HOOK(void* , WithliveApi_WithliveEnterWithHttpInfoAsync, (void* self, Il2cppUtils::Il2CppObject* request, void* cancellation_token, void* method_info)) {
        Shareable::renderScene = Shareable::RenderScene::WithLive;
        return WithliveApi_WithliveEnterWithHttpInfoAsync_Orig(self,
                                                             request,
                                                             cancellation_token, method_info);
    }

    DEFINE_HOOK(Il2cppUtils::Il2CppString* , AlstArchiveDirectory_GetLocalFullPathFromFileName, (Il2cppUtils::Il2CppObject* self, Il2cppUtils::Il2CppString* fileName)) {
        auto result = AlstArchiveDirectory_GetLocalFullPathFromFileName_Orig(self, fileName);
        auto result_str = result->ToString();
        if (Config::enableMotionCaptureReplay) {
            auto archive_id = Shareable::currentArchiveId;
            auto it = Config::archiveConfigMap.find(archive_id);
            if (it == Config::archiveConfigMap.end()) return result;
            auto archive_config = it->second;
            auto replay_type = archive_config["replay_type"].get<uint>();
            if (replay_type == 1 || replay_type == 2) { // replay
                auto new_result_str = replaceUriHost(result_str, Config::motionCaptureResourceUrl);
                result = Il2cppUtils::Il2CppString::New(new_result_str);
            }
        }
        return result;
    }

#pragma endregion

#pragma region oldVersion
    DEFINE_HOOK(void, Configuration_AddDefaultHeader, (void* self, Il2cppUtils::Il2CppString* key, Il2cppUtils::Il2CppString* value, void* mtd)) {
        if (Config::enableLegacyCompatibility) {
            Log::DebugFmt("Configuration_AddDefaultHeader HOOKED, %s=%s", key->ToString().c_str(), value->ToString().c_str());
            auto key_str = key->ToString();
            auto value_str = value->ToString();
            if (key_str == "x-client-version") {
                value = Il2cppUtils::Il2CppString::New(Config::latestClientVersion.toString());
            }
            if (key_str == "x-res-version") {
                value = Il2cppUtils::Il2CppString::New(Misc::StringFormat::split_once(Config::latestResVersion, "@").first);
            }
        }
        Configuration_AddDefaultHeader_Orig(self, key, value ,mtd);
    }

    DEFINE_HOOK(void, Configuration_set_UserAgent, (void* self, Il2cppUtils::Il2CppString* value, void* mtd)) {
        if (Config::enableLegacyCompatibility) {
            Log::DebugFmt("Configuration_set_UserAgent HOOKED, %s", value->ToString().c_str());
            auto value_str = value->ToString();
            if (value_str.starts_with("inspix-android")) {
                value = Il2cppUtils::Il2CppString::New("inspix-android/" + Config::latestClientVersion.toString());
            }
        }
        Configuration_set_UserAgent_Orig(self, value ,mtd);
    }

    DEFINE_HOOK(void, ApiClient_ctor_string, (void* self, Il2cppUtils::Il2CppString* basePath, void* mtd)) {
        if (!Config::enableOfflineApiMock) {
            ApiClient_ctor_string_Orig(self, basePath, mtd);
            return;
        }
        ApiClient_ctor_string_Orig(self, RewriteApiBasePathString(basePath, "ApiClient.ctor(string)"), mtd);
    }

    DEFINE_HOOK(void, Configuration_ctor_dictionaries, (void* self, void* defaultHeader, void* apiKey, void* apiKeyPrefix, Il2cppUtils::Il2CppString* basePath, void* mtd)) {
        if (!Config::enableOfflineApiMock) {
            Configuration_ctor_dictionaries_Orig(self, defaultHeader, apiKey, apiKeyPrefix, basePath, mtd);
            return;
        }
        Configuration_ctor_dictionaries_Orig(self,
                                             defaultHeader,
                                             apiKey,
                                             apiKeyPrefix,
                                             RewriteApiBasePathString(basePath, "Configuration.ctor(..., string)"),
                                             mtd);
    }

    DEFINE_HOOK(Il2cppUtils::Il2CppString*, Configuration_get_BasePath, (void* self, void* mtd)) {
        auto result = Configuration_get_BasePath_Orig(self, mtd);
        if (Config::enableOfflineApiMock) {
            return RewriteApiBasePathString(result, "Configuration.get_BasePath");
        }
        return result;
    }

    DEFINE_HOOK(void, Configuration_set_BasePath, (void* self, Il2cppUtils::Il2CppString* value, void* mtd)) {
        if (!Config::enableOfflineApiMock) {
            Configuration_set_BasePath_Orig(self, value, mtd);
            return;
        }
        Configuration_set_BasePath_Orig(self, RewriteApiBasePathString(value, "Configuration.set_BasePath"), mtd);
    }

    DEFINE_HOOK(void, WebViewCtrl_LoadView, (void* self, Il2cppUtils::Il2CppString* url, void* mtd)) {
        WebViewCtrl_LoadView_Orig(self, RewriteWebViewUrlString(url, "Tecotec.WebViewCtrl.LoadView"), mtd);
    }

    DEFINE_HOOK(void, WebViewObject_LoadURL, (void* self, Il2cppUtils::Il2CppString* url, void* mtd)) {
        WebViewObject_LoadURL_Orig(self, RewriteWebViewUrlString(url, "WebViewObject.LoadURL"), mtd);
    }

    DEFINE_HOOK(void, WebViewObject_LoadHTML, (void* self, Il2cppUtils::Il2CppString* html, Il2cppUtils::Il2CppString* baseUrl, void* mtd)) {
        auto rewrittenBaseUrl = RewriteWebViewUrlString(baseUrl, "WebViewObject.LoadHTML");
        WebViewObject_LoadHTML_Orig(self, html, rewrittenBaseUrl, mtd);
    }

    DEFINE_HOOK(void, CommonApiCache_set__LauncherInfo, (void* self, Il2cppUtils::Il2CppString* value, void* mtd)) {
        if (Config::dbgMode || Config::enableOfflineApiMock) {
            Log::InfoFmt("[CommonApiCache] set__LauncherInfo value=%s",
                         value ? value->ToString().c_str() : "(null)");
        }
        CommonApiCache_set__LauncherInfo_Orig(self, value, mtd);
    }

    DEFINE_HOOK(void, CommonApiCache_UpdateFromCommonHeader, (void* self, void* header, void* mtd)) {
        const auto headerJson = ObjectToJsonOrString(header);
        const auto normalizedCount = AddLowercaseStringHeaderAliases(header, headerJson);
        if (Config::dbgMode || Config::enableOfflineApiMock) {
            auto klass = header ? Il2cppUtils::get_class_from_instance(header) : nullptr;
            Log::InfoFmt("[CommonApiCache] UpdateFromCommonHeader self=%p header=%p type=%s.%s",
                         self,
                         header,
                         klass && klass->namespaze ? klass->namespaze : "",
                         klass && klass->name ? klass->name : "");
            Log::InfoFmt("[CommonApiCache] header=%s", headerJson.dump().c_str());
            if (normalizedCount > 0) {
                Log::InfoFmt("[CommonApiCache] added lowercase header aliases=%d", normalizedCount);
            }
        }
        CommonApiCache_UpdateFromCommonHeader_Orig(self, header, mtd);
    }

    DEFINE_HOOK(int32_t, CommonApiCache_GetLauncherInfoStatus, (void* self, Il2cppUtils::Il2CppString* key, void* mtd)) {
        auto result = CommonApiCache_GetLauncherInfoStatus_Orig(self, key, mtd);
        if (Config::dbgMode || Config::enableOfflineApiMock) {
            Log::InfoFmt("[CommonApiCache] GetLauncherInfoStatus key=%s result=%d",
                         key ? key->ToString().c_str() : "(null)",
                         result);
        }
        return result;
    }

//    DEFINE_HOOK(void, AssetManager_SynchronizeResourceVersion_MoveNext, (void* self, void* mtd)) {
////        Log::DebugFmt("AssetManager_SynchronizeResourceVersion HOOKED, requestedVersion is %s", requestedVersion->ToString().c_str());
//        static auto AssetManager_klass = Il2cppUtils::GetClassIl2cpp("Core.dll", "Hailstorm", "AssetManager");
//        static auto SynchronizeResourceVersion_klass = Il2cppUtils::find_nested_class_from_name(AssetManager_klass, "<SynchronizeResourceVersion>d__22");
//        Log::DebugFmt("SynchronizeResourceVersion_klass is at %p", SynchronizeResourceVersion_klass);
//        if (SynchronizeResourceVersion_klass) {
//            static auto requestedVersion_field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>("il2cpp_class_get_field_from_name", SynchronizeResourceVersion_klass, "requestedVersion");
//            static auto savedVersion_field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>("il2cpp_class_get_field_from_name", SynchronizeResourceVersion_klass, "savedVersion");
//            Log::DebugFmt("requestedVersion_field is %p, savedVersion_field is %p", requestedVersion_field, savedVersion_field);
//            auto requestedVersion = Il2cppUtils::ClassGetFieldValue<Il2cppUtils::Il2CppString*>(self, requestedVersion_field);
//            auto savedVersion = Il2cppUtils::ClassGetFieldValue<Il2cppUtils::Il2CppString*>(self, savedVersion_field);
//            if (requestedVersion) {
//                auto requestedVersion_str = requestedVersion->ToString();
//                Log::DebugFmt("AssetManager_SynchronizeResourceVersion HOOKED, requestedVersion is %s", requestedVersion_str.c_str());
//            }
//            if (savedVersion) {
//                auto savedVersion_str = savedVersion->ToString();
//                Log::DebugFmt("AssetManager_SynchronizeResourceVersion HOOKED, savedVersion is %s",  savedVersion_str.c_str());
//            }
//        }
//        AssetManager_SynchronizeResourceVersion_MoveNext_Orig(self, mtd);
//    }

    // this will cause stuck
//    DEFINE_HOOK(void*, AssetManager_SynchronizeResourceVersion, (void* self, Il2cppUtils::Il2CppString* requestedVersion, Il2cppUtils::Il2CppString* savedVersion, void* mtd)) {
//        Log::DebugFmt("Hailstorm_AssetManager__SynchronizeResourceVersion HOOKED, requestedVersion is %s, savedVersion is %s", requestedVersion->ToString().c_str(), savedVersion->ToString().c_str());
////        auto hooked_requestedVersion = Il2cppUtils::Il2CppString::New("R2504300@hbZZOCoWTueF+rikQLgPapC2Qw==");
//        return AssetManager_SynchronizeResourceVersion_Orig(self, requestedVersion, savedVersion, mtd);
//    }

    // Core_SynchronizeResourceVersion -> AssetManager_SynchronizeResourceVersion
    DEFINE_HOOK(void* , Core_SynchronizeResourceVersion, (void* self, Il2cppUtils::Il2CppString* requestedVersion,  void* mtd)) {
        Log::DebugFmt("Core_SynchronizeResourceVersion HOOKED, requestedVersion is %s", requestedVersion->ToString().c_str());
        if (Config::enableLegacyCompatibility) {
            Log::DebugFmt("requestedVersion is changed from %s to %s", requestedVersion->ToString().c_str(), Config::currentResVersion.c_str());
            requestedVersion = Il2cppUtils::Il2CppString::New(Config::currentResVersion);
        }
        return Core_SynchronizeResourceVersion_Orig(self, requestedVersion, mtd);
    }
    DEFINE_HOOK(Il2cppUtils::Il2CppString*, Application_get_version, ()) {
        Il2cppUtils::Il2CppString* result = Application_get_version_Orig();
        if (Config::enableLegacyCompatibility) {
            Log::DebugFmt("Application_get_version HOOKED, version is changed from %s to %s", result->ToString().c_str(), Config::currentClientVersion.toString().c_str());
            result = Il2cppUtils::Il2CppString::New(Config::currentClientVersion.toString());
        }
        return result;
    }

    DEFINE_HOOK(void, ApiException_ctor_2, (void* self,
                                            int errorCode,
                                            Il2cppUtils::Il2CppString* message,
                                            void* method_info)) {
        ApiException_ctor_2_Orig(self, errorCode, message, method_info);
        AppendOfficialApiExceptionCtorDump(errorCode, message, nullptr, nullptr);
    }

    DEFINE_HOOK(void, ApiException_ctor_3, (void* self,
                                            int errorCode,
                                            Il2cppUtils::Il2CppString* message,
                                            void* errorContent,
                                            void* method_info)) {
        ApiException_ctor_3_Orig(self, errorCode, message, errorContent, method_info);
        AppendOfficialApiExceptionCtorDump(errorCode, message, errorContent, nullptr);
    }

    DEFINE_HOOK(void, ApiException_ctor_4, (void* self,
                                            int errorCode,
                                            Il2cppUtils::Il2CppString* message,
                                            void* errorContent,
                                            void* headers,
                                            void* method_info)) {
        ApiException_ctor_4_Orig(self, errorCode, message, errorContent, headers, method_info);
        AppendOfficialApiExceptionCtorDump(errorCode, message, errorContent, headers);
    }
#pragma region

    void Install(HookInstaller* hookInstaller) {
        if (Config::dbgMode || Config::enableOfflineApiMock) {
            Log::InfoFmt("[HttpMock] native build=%s %s", __DATE__, __TIME__);
        }

        // GetHttpAsyncAddr
        ADD_HOOK(ApiClient_CallApiAsync, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Client","ApiClient", "CallApiAsync"));
        ADD_HOOK(ApiClient_Deserialize, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Client","ApiClient", "Deserialize"));
        auto ApiException_klass = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "ApiException");
        if (ApiException_klass) {
            auto apiExceptionMethod = Il2cppUtils::GetMethodIl2cpp(ApiException_klass, ".ctor", 2);
            ADD_HOOK(ApiException_ctor_2, apiExceptionMethod ? apiExceptionMethod->methodPointer : 0);
            apiExceptionMethod = Il2cppUtils::GetMethodIl2cpp(ApiException_klass, ".ctor", 3);
            ADD_HOOK(ApiException_ctor_3, apiExceptionMethod ? apiExceptionMethod->methodPointer : 0);
            apiExceptionMethod = Il2cppUtils::GetMethodIl2cpp(ApiException_klass, ".ctor", 4);
            ADD_HOOK(ApiException_ctor_4, apiExceptionMethod ? apiExceptionMethod->methodPointer : 0);
        } else {
            Log::Warn("[OfficialApiDump] ApiException class not found");
        }
#pragma region ArchiveApi
        auto ArchiveApi_klass = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "ArchiveApi");
        auto method = (Il2cppUtils::MethodInfo*) nullptr;
        if (ArchiveApi_klass) {
            // hook /v1/archive/get_fes_archive_data response
            auto ArchiveGetFesArchiveDataWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(ArchiveApi_klass, "<ArchiveGetFesArchiveDataWithHttpInfoAsync>d__30");
            method = Il2cppUtils::GetMethodIl2cpp(ArchiveGetFesArchiveDataWithHttpInfoAsync_klass, "MoveNext", 0);
            if (method) {
                ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync_MoveNext_Addr = method->methodPointer;
            }
            // hook /v1/archive/get_with_archive_data response
            auto ArchiveGetWithArchiveDataWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(ArchiveApi_klass, "<ArchiveGetWithArchiveDataWithHttpInfoAsync>d__46");
            method = Il2cppUtils::GetMethodIl2cpp(ArchiveGetWithArchiveDataWithHttpInfoAsync_klass, "MoveNext", 0);
            if (method) {
                ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync_MoveNext_Addr = method->methodPointer;
            }
            // hook /v1/archive/get_fes_archive_data response 1.x.x
            auto ArchiveGetFesArchiveDataWithHttpInfoAsync_old_klass = Il2cppUtils::find_nested_class_from_name(ArchiveApi_klass, "<ArchiveGetFesArchiveDataWithHttpInfoAsync>d__34");
            method = Il2cppUtils::GetMethodIl2cpp(ArchiveGetFesArchiveDataWithHttpInfoAsync_old_klass, "MoveNext", 0);
            if (method) {
                ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync_Old_MoveNext_Addr = method->methodPointer;
            }
            // hook /v1/archive/get_with_archive_data response 1.x.x
            auto ArchiveGetWithArchiveDataWithHttpInfoAsync_old_klass = Il2cppUtils::find_nested_class_from_name(ArchiveApi_klass, "<ArchiveGetWithArchiveDataWithHttpInfoAsync>d__50");
            method = Il2cppUtils::GetMethodIl2cpp(ArchiveGetWithArchiveDataWithHttpInfoAsync_old_klass, "MoveNext", 0);
            if (method) {
                ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync_Old_MoveNext_Addr = method->methodPointer;
            }
            // hook /v1/archive/get_archive_list response
            auto ArchiveGetArchiveListWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(ArchiveApi_klass, "<ArchiveGetArchiveListWithHttpInfoAsync>d__18");
            method = Il2cppUtils::GetMethodIl2cpp(ArchiveGetArchiveListWithHttpInfoAsync_klass, "MoveNext", 0);
            if (method) {
                ArchiveApi_ArchiveGetArchiveList_MoveNext_Addr = method->methodPointer;
            }
            // hook /v1/archive/get_archive_list response 1.x.x
            auto ArchiveGetArchiveListWithHttpInfoAsync_old_klass = Il2cppUtils::find_nested_class_from_name(ArchiveApi_klass, "<ArchiveGetArchiveListWithHttpInfoAsync>d__22");
            method = Il2cppUtils::GetMethodIl2cpp(ArchiveGetArchiveListWithHttpInfoAsync_old_klass, "MoveNext", 0);
            if (method) {
                ArchiveApi_ArchiveGetArchiveList_Old_MoveNext_Addr = method->methodPointer;
            }
            const bool usesSeparatedWithliveInfoArgs = VersionCompatibility::VersionChecker(">= 5.0.0").checkCompatibility(Config::currentClientVersion);
            auto ArchiveWithliveInfoWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(
                    ArchiveApi_klass,
                    usesSeparatedWithliveInfoArgs ? "<ArchiveWithliveInfoWithHttpInfoAsync>d__82" : "<ArchiveWithliveInfoWithHttpInfoAsync>d__70");
            method = Il2cppUtils::GetMethodIl2cpp(ArchiveWithliveInfoWithHttpInfoAsync_klass, "MoveNext", 0);
            if (method) {
                ArchiveApi_ArchiveWithliveInfoWithHttpInfoAsync_MoveNext_Addr = method->methodPointer;
            }
        }
        // Fes live camera unlock
        ADD_HOOK(ArchiveApi_ArchiveSetFesCameraWithHttpInfoAsync, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "ArchiveApi", "ArchiveSetFesCameraWithHttpInfoAsync"));
#pragma endregion

#pragma region WithliveApi
        auto WithliveApi_klass = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "WithliveApi");
        method = (Il2cppUtils::MethodInfo*) nullptr;
        if (WithliveApi_klass) {
            // hook /v1/withlive/enter response
            auto WithliveEnterWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(
                    WithliveApi_klass, "<WithliveEnterWithHttpInfoAsync>d__30");
            method = Il2cppUtils::GetMethodIl2cpp(WithliveEnterWithHttpInfoAsync_klass, "MoveNext",
                                                  0);
            if (method) {
                WithliveApi_WithliveEnterWithHttpInfoAsync_MoveNext_Addr = method->methodPointer;
            }
            // hook /v1/withlive/live_info response
            auto WithliveLiveInfoWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(
                    WithliveApi_klass, "<WithliveLiveInfoWithHttpInfoAsync>d__50");
            method = Il2cppUtils::GetMethodIl2cpp(WithliveLiveInfoWithHttpInfoAsync_klass, "MoveNext",
                                                  0);
            if (method) {
                WithliveApi_WithliveLiveInfoWithHttpInfoAsync_MoveNext_Addr = method->methodPointer;
            }
        }
#pragma endregion

#pragma region FesliveApi
        auto FesliveApi_klass = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "FesliveApi");
        if (FesliveApi_klass) {
            // hook /v1/withlive/enter response
            auto FesliveEnterWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(
                    FesliveApi_klass, "<FesliveEnterWithHttpInfoAsync>d__38");
            method = Il2cppUtils::GetMethodIl2cpp(FesliveEnterWithHttpInfoAsync_klass, "MoveNext", 0);
            if (method) {
                FesliveApi_FesliveEnterWithHttpInfoAsync_MoveNext_Addr = method->methodPointer;
            }
            // hook /v1/feslive/live_info response
            auto FesliveLiveInfoWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(
                    FesliveApi_klass, "<FesliveLiveInfoWithHttpInfoAsync>d__66");
            method = Il2cppUtils::GetMethodIl2cpp(FesliveLiveInfoWithHttpInfoAsync_klass, "MoveNext", 0);
            if (method) {
                FesliveApi_FesliveLiveInfoWithHttpInfoAsync_MoveNext_Addr = method->methodPointer;
            }
        }
        // Fes live camera unlock
        ADD_HOOK(FesliveApi_FesliveSetCameraWithHttpInfoAsync, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "FesliveApi", "FesliveSetCameraWithHttpInfoAsync"));
#pragma endregion

#pragma region WebviewLiveApi
        auto WebviewLiveApi_klass = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "WebviewLiveApi");
        if (WebviewLiveApi_klass) {
            // hook /v1/webview/live/live_info
            auto WebviewLiveLiveInfoWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(
                    WebviewLiveApi_klass, "<WebviewLiveLiveInfoWithHttpInfoAsync>d__22");
            method = Il2cppUtils::GetMethodIl2cpp(WebviewLiveLiveInfoWithHttpInfoAsync_klass, "MoveNext", 0);
            if (method) {
                WebviewLiveApi_WebviewLiveLiveInfoWithHttpInfoAsync_MoveNext_Addr = method->methodPointer;
            }
        }
#pragma endregion
#pragma region ActivityTopApi
        auto ActivityTopApi_klass = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "ActivityRecordApi");
        method = (Il2cppUtils::MethodInfo*) nullptr;
        if (ActivityTopApi_klass) {
            auto ActivityRecordGetTopWithHttpInfoAsync_klass = Il2cppUtils::find_nested_class_from_name(
                    ActivityTopApi_klass, "<ActivityRecordGetTopWithHttpInfoAsync>d__18");
            method = Il2cppUtils::GetMethodIl2cpp(ActivityRecordGetTopWithHttpInfoAsync_klass, "MoveNext", 0);
            if (method) {
                ActivityRecordGetTopWithHttpInfoAsync_MoveNext_Addr = method->methodPointer;
            }
        }

#pragma region RenderScene
        ADD_HOOK(AlstArchiveDirectory_GetLocalFullPathFromFileName, Il2cppUtils::GetMethodPointer("Core.dll", "Alstromeria", "AlstArchiveDirectory", "GetRemoteUriFromFileName"));
        ADD_HOOK(FesliveApi_FesliveEnterWithHttpInfoAsync , Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "FesliveApi", "FesliveEnterWithHttpInfoAsync"));
        ADD_HOOK(WithliveApi_WithliveEnterWithHttpInfoAsync , Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "WithliveApi", "WithliveEnterWithHttpInfoAsync"));
        ADD_HOOK(ArchiveApi_ArchiveGetFesArchiveDataWithHttpInfoAsync, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "ArchiveApi", "ArchiveGetFesArchiveDataWithHttpInfoAsync"));
        ADD_HOOK(ArchiveApi_ArchiveGetWithArchiveDataWithHttpInfoAsync, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "ArchiveApi", "ArchiveGetWithArchiveDataWithHttpInfoAsync"));
        ADD_HOOK(ArchiveApi_ArchiveGetArchiveListWithHttpInfoAsync, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Api", "ArchiveApi", "ArchiveGetArchiveListWithHttpInfoAsync"));
#pragma endregion
#pragma region oldVersion
        ADD_HOOK(Configuration_AddDefaultHeader, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "Configuration", "AddDefaultHeader"));
        ADD_HOOK(Configuration_set_UserAgent, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "Configuration", "set_UserAgent"));
        ADD_HOOK(ApiClient_ctor_string, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "ApiClient", ".ctor", {"System.String"}));
        method = Il2cppUtils::GetMethodIl2cpp("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "Configuration", ".ctor", 4);
        ADD_HOOK(Configuration_ctor_dictionaries, method ? method->methodPointer : 0);
        ADD_HOOK(Configuration_get_BasePath, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "Configuration", "get_BasePath"));
        ADD_HOOK(Configuration_set_BasePath, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Org.OpenAPITools.Client", "Configuration", "set_BasePath"));
        ADD_HOOK(WebViewCtrl_LoadView, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Tecotec", "WebViewCtrl", "LoadView"));
        ADD_HOOK(WebViewObject_LoadURL, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "", "WebViewObject", "LoadURL"));
        ADD_HOOK(WebViewObject_LoadHTML, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "", "WebViewObject", "LoadHTML"));
        auto CommonApiCache_klass = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "Tecotec", "CommonApiCache");
        if (!CommonApiCache_klass) {
            CommonApiCache_klass = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "", "CommonApiCache");
        }
        if (!CommonApiCache_klass) {
            Log::WarnFmt("[CommonApiCache] class not found");
        } else {
            auto updateFromCommonHeader = Il2cppUtils::GetMethodIl2cpp(CommonApiCache_klass, "UpdateFromCommonHeader", 1);
            auto setLauncherInfo = Il2cppUtils::GetMethodIl2cpp(CommonApiCache_klass, "set__LauncherInfo", 1);
            auto getLauncherInfoStatus = Il2cppUtils::GetMethodIl2cpp(CommonApiCache_klass, "GetLauncherInfoStatus", 1);
            ADD_HOOK(CommonApiCache_UpdateFromCommonHeader, updateFromCommonHeader ? updateFromCommonHeader->methodPointer : 0);
            ADD_HOOK(CommonApiCache_set__LauncherInfo, setLauncherInfo ? setLauncherInfo->methodPointer : 0);
            ADD_HOOK(CommonApiCache_GetLauncherInfoStatus, getLauncherInfoStatus ? getLauncherInfoStatus->methodPointer : 0);
            Log::InfoFmt("[CommonApiCache] hooks update=%p setLauncherInfo=%p getStatus=%p",
                         updateFromCommonHeader ? reinterpret_cast<void*>(updateFromCommonHeader->methodPointer) : nullptr,
                         setLauncherInfo ? reinterpret_cast<void*>(setLauncherInfo->methodPointer) : nullptr,
                         getLauncherInfoStatus ? reinterpret_cast<void*>(getLauncherInfoStatus->methodPointer) : nullptr);
        }
//        ADD_HOOK(AssetManager_SynchronizeResourceVersion, Il2cppUtils::GetMethodPointer("Core.dll", "Hailstorm", "AssetManager", "SynchronizeResourceVersion"));
        ADD_HOOK(Core_SynchronizeResourceVersion, Il2cppUtils::GetMethodPointer("Core.dll", "", "Core", "SynchronizeResourceVersion"));
        ADD_HOOK(Application_get_version, Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Application::get_version"));
        //        auto AssetManager_klass = Il2cppUtils::GetClassIl2cpp("Core.dll", "Hailstorm", "AssetManager");
//        if (AssetManager_klass) {
//            auto SynchronizeResourceVersion_klss = Il2cppUtils::find_nested_class_from_name(AssetManager_klass, "<SynchronizeResourceVersion>d__22");
//            method = Il2cppUtils::GetMethodIl2cpp(SynchronizeResourceVersion_klss, "MoveNext", 0);
//            if (method) {
//                ADD_HOOK(AssetManager_SynchronizeResourceVersion_MoveNext, method->methodPointer);
//            }
//        }
        ADD_HOOK(Silverflame_SFL_AdvSeriesMaster_ctor, Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "AdvSeriesMaster", ".ctor"));
        ADD_HOOK(Silverflame_SFL_AdvSeriesMaster_Fetch, Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "AdvSeriesMaster", "Fetch"));
        ADD_HOOK(Silverflame_SFL_AdvDatasMaster_ctor, Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "AdvDatasMaster", ".ctor"));
        ADD_HOOK(Silverflame_SFL_AdvDatasMaster_Fetch, Il2cppUtils::GetMethodPointer("Core.dll", "Silverflame.SFL", "AdvDatasMaster", "Fetch"));
#pragma endregion
    }
}
