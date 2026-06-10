#include "HttpMock.hpp"

#include "../HookMain.h"

#include <ctime>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

extern jclass g_linkuraHookMainClass;

namespace LinkuraLocal::HttpMock {
    namespace {
        static std::string TrimAscii(std::string s) {
            auto isSpace = [](unsigned char c) {
                return c == ' ' || c == '\t' || c == '\r' || c == '\n';
            };
            while (!s.empty() && isSpace((unsigned char)s.front())) s.erase(s.begin());
            while (!s.empty() && isSpace((unsigned char)s.back())) s.pop_back();
            return s;
        }

        static bool IEqualsAscii(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                auto ca = (unsigned char)a[i];
                auto cb = (unsigned char)b[i];
                if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
                if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
                if (ca != cb) return false;
            }
            return true;
        }

        static std::string FormatHttpDateNowGmt() {
            std::time_t now = std::time(nullptr);
            std::tm tmUtc{};
#if defined(_WIN32)
            gmtime_s(&tmUtc, &now);
#else
            gmtime_r(&now, &tmUtc);
#endif
            char buf[64]{};
            std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tmUtc);
            return std::string(buf);
        }

        static std::vector<std::pair<std::string, std::string>> ParseHeadersText(std::string text) {
            std::vector<std::pair<std::string, std::string>> out;
            size_t start = 0;
            while (start <= text.size()) {
                size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                auto line = text.substr(start, end - start);
                start = end + 1;

                line = TrimAscii(std::move(line));
                if (line.empty()) continue;
                if (line.rfind("#", 0) == 0) continue;
                if (line.rfind("//", 0) == 0) continue;

                auto pos = line.find(':');
                if (pos == std::string::npos) continue;
                auto key = TrimAscii(line.substr(0, pos));
                auto val = TrimAscii(line.substr(pos + 1));
                if (key.empty()) continue;
                out.emplace_back(std::move(key), std::move(val));
            }
            return out;
        }

        static void UpsertHeader(std::vector<std::pair<std::string, std::string>>& headers,
                                 const std::string& key,
                                 const std::string& value) {
            for (auto& kv : headers) {
                if (IEqualsAscii(kv.first, key)) {
                    kv.second = value;
                    return;
                }
            }
            headers.emplace_back(key, value);
        }

        static void ApplyStandardHeaders(std::vector<std::pair<std::string, std::string>>& headers) {
            const auto dateStr = FormatHttpDateNowGmt();
            const auto resVer = !Config::latestResVersion.empty() ? Config::latestResVersion : Config::currentResVersion;

            UpsertHeader(headers, "content-type", "application/json; charset=UTF-8");
            UpsertHeader(headers, "x-res-version", resVer.empty() ? std::string("unknown") : resVer);
            UpsertHeader(headers, "x-server-date", dateStr);
            UpsertHeader(headers, "date", dateStr);
            UpsertHeader(headers, "server", "Google Frontend");
            UpsertHeader(headers, "Transfer-Encoding", "chunked");
        }

        struct Methods {
            void* restResponseKlass = nullptr;
            Il2cppUtils::MethodInfo* restResponseCtor = nullptr;
            Il2cppUtils::MethodInfo* setContent = nullptr;
            Il2cppUtils::MethodInfo* setContentType = nullptr;
            Il2cppUtils::MethodInfo* setStatusCode = nullptr;
            Il2cppUtils::MethodInfo* setStatusDescription = nullptr;
            Il2cppUtils::MethodInfo* setResponseStatus = nullptr;
            Il2cppUtils::MethodInfo* setContentLength = nullptr;
            Il2cppUtils::MethodInfo* setRawBytes = nullptr;
            Il2cppUtils::MethodInfo* setHeaders = nullptr;

            // For RestResponse.Headers population (List<Parameter>).
            void* listOfParamKlass = nullptr; // System.Collections.Generic.List`1[[RestSharp.Parameter, RestSharp]]
            Il2cppUtils::MethodInfo* listCtor = nullptr;
            Il2cppUtils::MethodInfo* listAdd = nullptr;
            void* parameterKlass = nullptr; // RestSharp.HeaderParameter or RestSharp.Parameter
            Il2cppUtils::MethodInfo* parameterCtor = nullptr; // best-effort resolved ctor
            int parameterCtorArgs = -1;
            Il2cppUtils::MethodInfo* parameterCtor0 = nullptr;
            Il2cppUtils::MethodInfo* parameterSetType = nullptr;
            Il2cppUtils::MethodInfo* parameterSetName = nullptr;
            Il2cppUtils::MethodInfo* parameterSetValue = nullptr;
            void* parameterTypeKlass = nullptr; // RestSharp.ParameterType (enum)
            int parameterTypeHttpHeader = 2; // safe default

            // Completed Task<object> via TaskCompletionSource<object>
            void* taskCompletionSourceObjectKlass = nullptr; // Il2CppClass*
            Il2cppUtils::MethodInfo* tcsCtor = nullptr;
            Il2cppUtils::MethodInfo* tcsSetResult = nullptr;
            Il2cppUtils::MethodInfo* tcsGetTask = nullptr;

            UnityResolve::Method* restResponseGetContent = nullptr;
            UnityResolve::Method* restResponseGetStatusCode = nullptr;
            UnityResolve::Method* restResponseGetResponseStatus = nullptr;
            UnityResolve::Method* restResponseGetHeaders = nullptr;

            // For setting RawBytes safely.
            UnityResolve::Method* encodingGetUtf8 = nullptr;
            UnityResolve::Method* encodingGetBytes = nullptr;
        };

        static int ResolveEnumValue(void* enumKlass, const char* fieldName, int fallback) {
            if (!enumKlass || !fieldName) return fallback;
            Il2cppUtils::FieldInfo* field = nullptr;
            void* iter = nullptr;
            while ((field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>("il2cpp_class_get_fields", enumKlass, &iter))) {
                if (!field->name) continue;
                if (std::strcmp(field->name, fieldName) != 0) continue;
                int value = fallback;
                UnityResolve::Invoke<void>("il2cpp_field_static_get_value", field, &value);
                return value;
            }
            return fallback;
        }

        static Il2cppUtils::MethodInfo* ResolveParameterCtor(void* parameterKlass, int* outArgs) {
            if (outArgs) *outArgs = -1;
            if (!parameterKlass) return nullptr;

            void* iter = nullptr;
            void* method = nullptr;
            while ((method = UnityResolve::Invoke<void*>("il2cpp_class_get_methods", parameterKlass, &iter))) {
                const auto name = UnityResolve::Invoke<const char*>("il2cpp_method_get_name", method);
                if (!name || std::string_view(name) != ".ctor") continue;

                const int argc = UnityResolve::Invoke<int>("il2cpp_method_get_param_count", method);
                if (argc != 4 && argc != 3 && argc != 2) continue;

                const auto p0 = UnityResolve::Invoke<void*>("il2cpp_method_get_param", method, 0);
                const auto p1 = UnityResolve::Invoke<void*>("il2cpp_method_get_param", method, 1);
                const auto t0 = p0 ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", p0) : nullptr;
                const auto t1 = p1 ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", p1) : nullptr;
                if (!t0 || std::string_view(t0).find("System.String") == std::string_view::npos) continue;
                if (!t1) continue;
                const auto sv1 = std::string_view(t1);
                if (sv1.find("System.Object") == std::string_view::npos && sv1.find("System.String") == std::string_view::npos) {
                    continue;
                }

                // Favor (string, object|string, ParameterType, bool) ctor.
                if (argc == 4) {
                    const auto p2 = UnityResolve::Invoke<void*>("il2cpp_method_get_param", method, 2);
                    const auto p3 = UnityResolve::Invoke<void*>("il2cpp_method_get_param", method, 3);
                    const auto t2 = p2 ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", p2) : nullptr;
                    const auto t3 = p3 ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", p3) : nullptr;
                    if (!t2 || !t3) continue;
                    const auto sv2 = std::string_view(t2);
                    const auto sv3 = std::string_view(t3);
                    const bool ok2 = sv2.find("RestSharp.ParameterType") != std::string_view::npos || sv2.find("ParameterType") != std::string_view::npos;
                    const bool ok3 = sv3.find("System.Boolean") != std::string_view::npos || sv3.find("Boolean") != std::string_view::npos;
                    if (!ok2 || !ok3) continue;
                    if (outArgs) *outArgs = 4;
                    return static_cast<Il2cppUtils::MethodInfo*>(method);
                }

                // Favor (string, object|string, ParameterType) ctor; otherwise (string, object|string).
                if (argc == 3) {
                    const auto p2 = UnityResolve::Invoke<void*>("il2cpp_method_get_param", method, 2);
                    const auto t2 = p2 ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", p2) : nullptr;
                    if (!t2) continue;
                    const auto sv2 = std::string_view(t2);
                    if (sv2.find("RestSharp.ParameterType") == std::string_view::npos && sv2.find("ParameterType") == std::string_view::npos) {
                        continue;
                    }
                    if (outArgs) *outArgs = 3;
                    return static_cast<Il2cppUtils::MethodInfo*>(method);
                }

                if (outArgs) *outArgs = 2;
                return static_cast<Il2cppUtils::MethodInfo*>(method);
            }
            return nullptr;
        }

        static void* TryGetClassIl2cpp(std::initializer_list<const char*> assemblies, const char* ns, const char* name) {
            for (const auto asmName : assemblies) {
                if (!asmName) continue;
                if (auto klass = Il2cppUtils::GetClassIl2cpp(asmName, ns, name)) return klass;
            }
            return nullptr;
        }

        static Il2cppUtils::MethodInfo* TryGetMethodIl2cpp(std::initializer_list<const char*> assemblies,
                                                           const char* ns,
                                                           const char* className,
                                                           const char* methodName,
                                                           int argsCount) {
            for (const auto asmName : assemblies) {
                if (!asmName) continue;
                if (auto mtd = Il2cppUtils::GetMethodIl2cpp(asmName, ns, className, methodName, argsCount)) return mtd;
            }
            return nullptr;
        }

        static Methods& GetMethods() {
            static Methods methods{};
            static bool inited = false;
            if (inited) return methods;
            inited = true;

            const auto restSharpAssemblies = { "RestSharp.dll", "RestSharp" };
            methods.restResponseKlass = TryGetClassIl2cpp(restSharpAssemblies, "RestSharp", "RestResponse");
            methods.restResponseCtor = TryGetMethodIl2cpp(restSharpAssemblies, "RestSharp", "RestResponse", ".ctor", 0);
            methods.setContent = TryGetMethodIl2cpp(restSharpAssemblies, "RestSharp", "RestResponseBase", "set_Content", 1);
            methods.setContentType = TryGetMethodIl2cpp(restSharpAssemblies, "RestSharp", "RestResponseBase", "set_ContentType", 1);
            methods.setStatusCode = TryGetMethodIl2cpp(restSharpAssemblies, "RestSharp", "RestResponseBase", "set_StatusCode", 1);
            methods.setStatusDescription = TryGetMethodIl2cpp(restSharpAssemblies, "RestSharp", "RestResponseBase", "set_StatusDescription", 1);
            methods.setResponseStatus = TryGetMethodIl2cpp(restSharpAssemblies, "RestSharp", "RestResponseBase", "set_ResponseStatus", 1);
            methods.setContentLength = TryGetMethodIl2cpp(restSharpAssemblies, "RestSharp", "RestResponseBase", "set_ContentLength", 1);
            methods.setRawBytes = TryGetMethodIl2cpp(restSharpAssemblies, "RestSharp", "RestResponseBase", "set_RawBytes", 1);
            methods.setHeaders = TryGetMethodIl2cpp(restSharpAssemblies, "RestSharp", "RestResponseBase", "set_Headers", 1);

            // Resolve header-related types/methods.
            methods.listOfParamKlass = Il2cppUtils::get_system_class_from_reflection_type_str(
                "System.Collections.Generic.List`1[[RestSharp.Parameter, RestSharp]]", "mscorlib");
            if (methods.listOfParamKlass) {
                methods.listCtor = Il2cppUtils::GetMethodIl2cpp(methods.listOfParamKlass, ".ctor", 0);
                methods.listAdd = Il2cppUtils::GetMethodIl2cpp(methods.listOfParamKlass, "Add", 1);
            }

            // Prefer HeaderParameter if available, otherwise fall back to Parameter.
            const auto headerParameterKlass = TryGetClassIl2cpp(restSharpAssemblies, "RestSharp", "HeaderParameter");
            const auto parameterKlass = TryGetClassIl2cpp(restSharpAssemblies, "RestSharp", "Parameter");
            methods.parameterKlass = headerParameterKlass ? headerParameterKlass : parameterKlass;
            methods.parameterTypeKlass = TryGetClassIl2cpp(restSharpAssemblies, "RestSharp", "ParameterType");
            methods.parameterTypeHttpHeader = ResolveEnumValue(methods.parameterTypeKlass, "HttpHeader", 2);
            if (methods.parameterKlass) {
                methods.parameterCtor = ResolveParameterCtor(methods.parameterKlass, &methods.parameterCtorArgs);
                if (!methods.parameterCtor && methods.parameterKlass == headerParameterKlass && parameterKlass) {
                    // Some RestSharp builds keep the usable ctor only on Parameter, not HeaderParameter.
                    methods.parameterKlass = parameterKlass;
                    methods.parameterCtor = ResolveParameterCtor(methods.parameterKlass, &methods.parameterCtorArgs);
                }
                methods.parameterCtor0 = Il2cppUtils::GetMethodIl2cpp(methods.parameterKlass, ".ctor", 0);
                methods.parameterSetType = Il2cppUtils::GetMethodIl2cpp(methods.parameterKlass, "set_Type", 1);
                methods.parameterSetName = Il2cppUtils::GetMethodIl2cpp(methods.parameterKlass, "set_Name", 1);
                methods.parameterSetValue = Il2cppUtils::GetMethodIl2cpp(methods.parameterKlass, "set_Value", 1);
            }

            methods.taskCompletionSourceObjectKlass = Il2cppUtils::get_system_class_from_reflection_type_str(
                "System.Threading.Tasks.TaskCompletionSource`1[[System.Object, mscorlib]]",
                "mscorlib");
            if (!methods.taskCompletionSourceObjectKlass) {
                methods.taskCompletionSourceObjectKlass = Il2cppUtils::get_system_class_from_reflection_type_str(
                    "System.Threading.Tasks.TaskCompletionSource`1[System.Object]",
                    "mscorlib");
            }
            if (methods.taskCompletionSourceObjectKlass) {
                methods.tcsCtor = Il2cppUtils::GetMethodIl2cpp(methods.taskCompletionSourceObjectKlass, ".ctor", 0);
                methods.tcsSetResult = Il2cppUtils::GetMethodIl2cpp(methods.taskCompletionSourceObjectKlass, "SetResult", 1);
                methods.tcsGetTask = Il2cppUtils::GetMethodIl2cpp(methods.taskCompletionSourceObjectKlass, "get_Task", 0);
            }

            methods.restResponseGetContent = Il2cppUtils::GetMethod("RestSharp.dll", "RestSharp", "RestResponseBase", "get_Content");
            if (!methods.restResponseGetContent) {
                methods.restResponseGetContent = Il2cppUtils::GetMethod("RestSharp", "RestSharp", "RestResponseBase", "get_Content");
            }
            methods.restResponseGetStatusCode = Il2cppUtils::GetMethod("RestSharp.dll", "RestSharp", "RestResponseBase", "get_StatusCode");
            if (!methods.restResponseGetStatusCode) {
                methods.restResponseGetStatusCode = Il2cppUtils::GetMethod("RestSharp", "RestSharp", "RestResponseBase", "get_StatusCode");
            }
            methods.restResponseGetResponseStatus = Il2cppUtils::GetMethod("RestSharp.dll", "RestSharp", "RestResponseBase", "get_ResponseStatus");
            if (!methods.restResponseGetResponseStatus) {
                methods.restResponseGetResponseStatus = Il2cppUtils::GetMethod("RestSharp", "RestSharp", "RestResponseBase", "get_ResponseStatus");
            }
            methods.restResponseGetHeaders = Il2cppUtils::GetMethod("RestSharp.dll", "RestSharp", "RestResponseBase", "get_Headers");
            if (!methods.restResponseGetHeaders) {
                methods.restResponseGetHeaders = Il2cppUtils::GetMethod("RestSharp", "RestSharp", "RestResponseBase", "get_Headers");
            }

            methods.encodingGetUtf8 = Il2cppUtils::GetMethod("mscorlib.dll", "System.Text", "Encoding", "get_UTF8");
            methods.encodingGetBytes = Il2cppUtils::GetMethod("mscorlib.dll", "System.Text", "Encoding", "GetBytes", { "System.String" });

            if (Config::dbgMode || Config::enableOfflineApiMock) {
                Log::InfoFmt(
                    "[HttpMock] resolve: RestResponse klass=%p ctor=%p setContent=%p TCS klass=%p ctor=%p SetResult=%p get_Task=%p",
                    methods.restResponseKlass,
                    methods.restResponseCtor ? (void*)methods.restResponseCtor->methodPointer : nullptr,
                    methods.setContent ? (void*)methods.setContent->methodPointer : nullptr,
                    methods.taskCompletionSourceObjectKlass,
                    methods.tcsCtor ? (void*)methods.tcsCtor->methodPointer : nullptr,
                    methods.tcsSetResult ? (void*)methods.tcsSetResult->methodPointer : nullptr,
                    methods.tcsGetTask ? (void*)methods.tcsGetTask->methodPointer : nullptr);

                Log::InfoFmt(
                    "[HttpMock] resolve: listKlass=%p listCtor=%p listAdd=%p paramKlass=%p paramCtor=%p paramCtorArgs=%d paramTypeHttpHeader=%d setHeaders=%p",
                    methods.listOfParamKlass,
                    methods.listCtor ? (void*)methods.listCtor->methodPointer : nullptr,
                    methods.listAdd ? (void*)methods.listAdd->methodPointer : nullptr,
                    methods.parameterKlass,
                    methods.parameterCtor ? (void*)methods.parameterCtor->methodPointer : nullptr,
                    methods.parameterCtorArgs,
                    methods.parameterTypeHttpHeader,
                    methods.setHeaders ? (void*)methods.setHeaders->methodPointer : nullptr);

                if (methods.setHeaders) {
                    auto p0 = UnityResolve::Invoke<void*>("il2cpp_method_get_param", methods.setHeaders, 0);
                    auto t0 = p0 ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", p0) : nullptr;
                    Log::InfoFmt("[HttpMock] set_Headers param type=%s", t0 ? t0 : "(null)");
                }
            }

            return methods;
        }

        static void* CreateHeaderParameter(const std::string& name, const std::string& value) {
            auto& m = GetMethods();
            if (!m.parameterKlass) return nullptr;

            auto obj = UnityResolve::Invoke<void*>("il2cpp_object_new", m.parameterKlass);
            if (!obj) return nullptr;

            auto nameStr = Il2cppUtils::Il2CppString::New(name);
            auto valueStr = Il2cppUtils::Il2CppString::New(value);

            if (m.parameterCtor && m.parameterCtorArgs == 4) {
                using Ctor4Fn = void(*)(void*, Il2cppUtils::Il2CppString*, void*, int, bool, Il2cppUtils::MethodInfo*);
                reinterpret_cast<Ctor4Fn>(m.parameterCtor->methodPointer)(
                    obj, nameStr, valueStr, m.parameterTypeHttpHeader, false, m.parameterCtor);
            } else if (m.parameterCtor && m.parameterCtorArgs == 3) {
                using Ctor3Fn = void(*)(void*, Il2cppUtils::Il2CppString*, void*, int, Il2cppUtils::MethodInfo*);
                reinterpret_cast<Ctor3Fn>(m.parameterCtor->methodPointer)(
                    obj, nameStr, valueStr, m.parameterTypeHttpHeader, m.parameterCtor);
            } else if (m.parameterCtor && m.parameterCtorArgs == 2) {
                using Ctor2Fn = void(*)(void*, Il2cppUtils::Il2CppString*, void*, Il2cppUtils::MethodInfo*);
                reinterpret_cast<Ctor2Fn>(m.parameterCtor->methodPointer)(obj, nameStr, valueStr, m.parameterCtor);
            } else if (m.parameterCtor0) {
                using Ctor0Fn = void(*)(void*, Il2cppUtils::MethodInfo*);
                reinterpret_cast<Ctor0Fn>(m.parameterCtor0->methodPointer)(obj, m.parameterCtor0);
            }

            // Best-effort: set fields via setters too (some builds ignore ctor args).
            if (m.parameterSetName) {
                using SetNameFn = void(*)(void*, Il2cppUtils::Il2CppString*, Il2cppUtils::MethodInfo*);
                reinterpret_cast<SetNameFn>(m.parameterSetName->methodPointer)(obj, nameStr, m.parameterSetName);
            }
            if (m.parameterSetValue) {
                using SetValueFn = void(*)(void*, void*, Il2cppUtils::MethodInfo*);
                reinterpret_cast<SetValueFn>(m.parameterSetValue->methodPointer)(obj, valueStr, m.parameterSetValue);
            }
            if (m.parameterSetType) {
                using SetTypeFn = void(*)(void*, int, Il2cppUtils::MethodInfo*);
                reinterpret_cast<SetTypeFn>(m.parameterSetType->methodPointer)(obj, m.parameterTypeHttpHeader, m.parameterSetType);
            }

            return obj;
        }

        static void* CreateDefaultHeadersList() {
            auto& m = GetMethods();
            if (!m.listOfParamKlass || !m.listCtor || !m.listAdd) return nullptr;

            using CtorFn = void(*)(void*, Il2cppUtils::MethodInfo*);
            using AddFn = void(*)(void*, void*, Il2cppUtils::MethodInfo*);

            auto list = UnityResolve::Invoke<void*>("il2cpp_object_new", m.listOfParamKlass);
            if (!list) return nullptr;
            reinterpret_cast<CtorFn>(m.listCtor->methodPointer)(list, m.listCtor);

            std::vector<std::pair<std::string, std::string>> headers;
            ApplyStandardHeaders(headers);

            for (const auto& kv : headers) {
                auto param = CreateHeaderParameter(kv.first, kv.second);
                if (!param) {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        Log::Warn("[HttpMock] failed to create RestSharp header Parameter objects; Headers may be empty.");
                    }
                    continue;
                }
                reinterpret_cast<AddFn>(m.listAdd->methodPointer)(list, param, m.listAdd);
            }

            return list;
        }

        static void* CreateHeadersListFromPairs(const std::vector<std::pair<std::string, std::string>>& headers) {
            auto& m = GetMethods();
            if (!m.listOfParamKlass || !m.listCtor || !m.listAdd) return nullptr;

            using CtorFn = void(*)(void*, Il2cppUtils::MethodInfo*);
            using AddFn = void(*)(void*, void*, Il2cppUtils::MethodInfo*);

            auto list = UnityResolve::Invoke<void*>("il2cpp_object_new", m.listOfParamKlass);
            if (!list) return nullptr;
            reinterpret_cast<CtorFn>(m.listCtor->methodPointer)(list, m.listCtor);

            int added = 0;
            for (const auto& kv : headers) {
                auto param = CreateHeaderParameter(kv.first, kv.second);
                if (!param) continue;
                reinterpret_cast<AddFn>(m.listAdd->methodPointer)(list, param, m.listAdd);
                ++added;
            }

            if (Config::dbgMode || Config::enableOfflineApiMock) {
                auto typed = reinterpret_cast<UnityResolve::UnityType::List<void*>*>(list);
                const int sz = typed ? typed->size : -1;
                Log::InfoFmt("[HttpMock] headers list add done attempted=%d added=%d list->size=%d", (int)headers.size(), added, sz);
            }

            return list;
        }

        static void* CreateRestResponse(const std::string& jsonBody,
                                        int httpStatusCode,
                                        const std::string& statusDescription,
                                        const std::vector<std::pair<std::string, std::string>>& headers) {
            auto& m = GetMethods();
            if (!m.restResponseKlass || !m.restResponseCtor || !m.setContent || !m.setContentType || !m.setStatusCode || !m.setStatusDescription || !m.setResponseStatus) {
                Log::Error("HttpMock: RestSharp methods not resolved.");
                return nullptr;
            }

            auto resp = UnityResolve::Invoke<void*>("il2cpp_object_new", m.restResponseKlass);
            if (!resp) return nullptr;

            using CtorFn = void(*)(void*, Il2cppUtils::MethodInfo*);
            reinterpret_cast<CtorFn>(m.restResponseCtor->methodPointer)(resp, m.restResponseCtor);

            using SetStringFn = void(*)(void*, Il2cppUtils::Il2CppString*, Il2cppUtils::MethodInfo*);
            using SetIntFn = void(*)(void*, int, Il2cppUtils::MethodInfo*);

            auto contentStr = Il2cppUtils::Il2CppString::New(jsonBody);
            reinterpret_cast<SetStringFn>(m.setContent->methodPointer)(resp, contentStr, m.setContent);
            reinterpret_cast<SetStringFn>(m.setContentType->methodPointer)(resp, Il2cppUtils::Il2CppString::New("application/json; charset=UTF-8"), m.setContentType);
            reinterpret_cast<SetIntFn>(m.setStatusCode->methodPointer)(resp, httpStatusCode, m.setStatusCode);
            reinterpret_cast<SetStringFn>(m.setStatusDescription->methodPointer)(resp, Il2cppUtils::Il2CppString::New(statusDescription), m.setStatusDescription);
            reinterpret_cast<SetIntFn>(m.setResponseStatus->methodPointer)(resp, 1, m.setResponseStatus);

            // Ensure Headers is non-null.
            if (m.setHeaders) {
                void* headersList = nullptr;
                if (!headers.empty()) {
                    headersList = CreateHeadersListFromPairs(headers);
                }
                if (!headersList) {
                    headersList = CreateDefaultHeadersList();
                }
                if (headersList) {
                    using SetObjFn = void(*)(void*, void*, Il2cppUtils::MethodInfo*);
                    reinterpret_cast<SetObjFn>(m.setHeaders->methodPointer)(resp, headersList, m.setHeaders);
                }
            }

            // Populate RawBytes/ContentLength.
            if (m.setRawBytes && m.setContentLength && m.encodingGetUtf8 && m.encodingGetUtf8->function && m.encodingGetBytes && m.encodingGetBytes->function) {
                using GetUtf8Fn = void*(*)(void* method_info);
                using GetBytesFn = void*(*)(void* selfEncoding, Il2cppUtils::Il2CppString* s, void* method_info);
                auto enc = reinterpret_cast<GetUtf8Fn>(m.encodingGetUtf8->function)(m.encodingGetUtf8->address);
                if (enc) {
                    auto bytes = reinterpret_cast<GetBytesFn>(m.encodingGetBytes->function)(enc, contentStr, m.encodingGetBytes->address);
                    if (bytes) {
                        using SetObjFn = void(*)(void*, void*, Il2cppUtils::MethodInfo*);
                        reinterpret_cast<SetObjFn>(m.setRawBytes->methodPointer)(resp, bytes, m.setRawBytes);
                        using SetI64Fn = void(*)(void*, int64_t, Il2cppUtils::MethodInfo*);
                        reinterpret_cast<SetI64Fn>(m.setContentLength->methodPointer)(resp, (int64_t)jsonBody.size(), m.setContentLength);
                    }
                }
            }

            if (m.restResponseGetContent && m.restResponseGetContent->function && (Config::dbgMode || Config::enableOfflineApiMock)) {
                using GetContentFn = Il2cppUtils::Il2CppString*(*)(void*, void*);
                auto content = reinterpret_cast<GetContentFn>(m.restResponseGetContent->function)(resp, m.restResponseGetContent->address);
                const auto contentLen = content ? (int)content->ToString().size() : -1;
                Log::InfoFmt("[HttpMock] RestResponse content length=%d", contentLen);
            }
            if ((Config::dbgMode || Config::enableOfflineApiMock) && m.restResponseGetStatusCode && m.restResponseGetStatusCode->function) {
                using GetIntFn = int(*)(void*, void*);
                auto sc = reinterpret_cast<GetIntFn>(m.restResponseGetStatusCode->function)(resp, m.restResponseGetStatusCode->address);
                Log::InfoFmt("[HttpMock] RestResponse StatusCode=%d", sc);
            }
            if ((Config::dbgMode || Config::enableOfflineApiMock) && m.restResponseGetResponseStatus && m.restResponseGetResponseStatus->function) {
                using GetIntFn = int(*)(void*, void*);
                auto rs = reinterpret_cast<GetIntFn>(m.restResponseGetResponseStatus->function)(resp, m.restResponseGetResponseStatus->address);
                Log::InfoFmt("[HttpMock] RestResponse ResponseStatus=%d", rs);
            }
            if ((Config::dbgMode || Config::enableOfflineApiMock) && m.restResponseGetHeaders && m.restResponseGetHeaders->function) {
                using GetObjFn = void*(*)(void*, void*);
                auto headers = reinterpret_cast<GetObjFn>(m.restResponseGetHeaders->function)(resp, m.restResponseGetHeaders->address);
                Log::InfoFmt("[HttpMock] RestResponse Headers=%p", headers);
            }

            return resp;
        }

        static void* TaskFromResultObject(void* resultObject) {
            auto& m = GetMethods();
            if (!m.taskCompletionSourceObjectKlass || !m.tcsCtor || !m.tcsSetResult || !m.tcsGetTask) {
                Log::Error("HttpMock: TaskCompletionSource<object> not resolved.");
                return nullptr;
            }

            UnityResolve::ThreadAttach();

            auto tcs = UnityResolve::Invoke<void*>("il2cpp_object_new", m.taskCompletionSourceObjectKlass);
            if (!tcs) {
                Log::Error("HttpMock: il2cpp_object_new(TaskCompletionSource<object>) failed.");
                return nullptr;
            }

            using CtorFn = void(*)(void*, Il2cppUtils::MethodInfo*);
            reinterpret_cast<CtorFn>(m.tcsCtor->methodPointer)(tcs, m.tcsCtor);

            using SetObjFn = void(*)(void*, void*, Il2cppUtils::MethodInfo*);
            reinterpret_cast<SetObjFn>(m.tcsSetResult->methodPointer)(tcs, resultObject, m.tcsSetResult);

            using GetObjFn = void*(*)(void*, Il2cppUtils::MethodInfo*);
            auto task = reinterpret_cast<GetObjFn>(m.tcsGetTask->methodPointer)(tcs, m.tcsGetTask);
            if (!task) {
                Log::Error("HttpMock: TaskCompletionSource<object>.Task returned nullptr.");
                return nullptr;
            }

            if (Config::dbgMode || Config::enableOfflineApiMock) {
                auto k = Il2cppUtils::get_class_from_instance(task);
                Log::InfoFmt("[HttpMock] created completed task=%p klass=%s.%s",
                             task,
                             (k && k->namespaze) ? k->namespaze : "",
                             (k && k->name) ? k->name : "");
            }
            return task;
        }

        static std::string FetchSelfhostApiJson(const std::string& baseUrl,
                                                const std::string& apiPath,
                                                const std::string& requestBodyJson) {
            auto env = Misc::GetJNIEnv();
            if (!env) {
                Log::Error("[HttpMock] failed to get JNIEnv for selfhost API.");
                return {};
            }

            auto klass = g_linkuraHookMainClass;
            if (!klass) {
                Log::Error("[HttpMock] LinkuraHookMain global class is null for selfhost API.");
                return {};
            }

            auto methodId = env->GetStaticMethodID(
                    klass,
                    "fetchSelfhostApi",
                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
            if (!methodId) {
                env->ExceptionClear();
                Log::Error("[HttpMock] failed to resolve fetchSelfhostApi.");
                return {};
            }

            auto jBaseUrl = env->NewStringUTF(baseUrl.c_str());
            auto jApiPath = env->NewStringUTF(apiPath.c_str());
            auto jRequestBodyJson = env->NewStringUTF(requestBodyJson.c_str());
            auto result = static_cast<jstring>(env->CallStaticObjectMethod(
                    klass, methodId, jBaseUrl, jApiPath, jRequestBodyJson));

            std::string body;
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
                Log::ErrorFmt("[HttpMock] selfhost API call failed path=%s", apiPath.c_str());
            } else if (result) {
                const char* chars = env->GetStringUTFChars(result, nullptr);
                if (chars) {
                    body = chars;
                    env->ReleaseStringUTFChars(result, chars);
                }
            }

            if (result) env->DeleteLocalRef(result);
            env->DeleteLocalRef(jRequestBodyJson);
            env->DeleteLocalRef(jApiPath);
            env->DeleteLocalRef(jBaseUrl);
            return body;
        }

    } // namespace

    void* CreateSelfhostApiTask(const std::string& baseUrl, const std::string& apiPath, const std::string& requestBodyJson) {
        const auto jsonBody = FetchSelfhostApiJson(baseUrl, apiPath, requestBodyJson);
        if (jsonBody.empty()) {
            Log::ErrorFmt("[HttpMock] empty selfhost API response path=%s", apiPath.c_str());
            return nullptr;
        }

        std::vector<std::pair<std::string, std::string>> headerPairs;
        ApplyStandardHeaders(headerPairs);
        auto resp = CreateRestResponse(jsonBody, 200, "OK (selfhost)", headerPairs);
        if (!resp) {
            Log::ErrorFmt("[HttpMock] failed to create selfhost RestResponse path=%s", apiPath.c_str());
            return nullptr;
        }
        Log::WarnFmt("[SelfhostAudit] selfhost_api_response path=%s bytes=%d", apiPath.c_str(), (int)jsonBody.size());
        return TaskFromResultObject(resp);
    }

}
