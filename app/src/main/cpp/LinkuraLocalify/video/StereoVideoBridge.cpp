#include "StereoVideoBridge.hpp"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace {
    constexpr const char* kLogTag = "StereoVideoBridge";
    thread_local bool g_tlsInSwapHook = false;
    std::atomic<bool> g_captureFrameReady{false};

    struct StereoVideoState {
        std::mutex mutex;

        ANativeWindow* window = nullptr;
        int targetWidth = 0;
        int targetHeight = 0;
        bool surfaceDirty = false;

        EGLDisplay eglDisplay = EGL_NO_DISPLAY;
        EGLSurface eglSurface = EGL_NO_SURFACE;
        EGLContext eglContext = EGL_NO_CONTEXT;

        GLuint program = 0;
        GLuint vertexShader = 0;
        GLuint fragmentShader = 0;
        GLuint captureTexture = 0;
        int captureWidth = 0;
        int captureHeight = 0;

        PFNEGLPRESENTATIONTIMEANDROIDPROC presentationTimeProc = nullptr;
    } g_state;

    void logError(const char* msg) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", msg);
    }

    GLuint compileShader(GLenum type, const char* source) {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint status = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (status == GL_TRUE) {
            return shader;
        }

        char logBuffer[512] = {0};
        glGetShaderInfoLog(shader, sizeof(logBuffer), nullptr, logBuffer);
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Shader compile failed: %s", logBuffer);
        glDeleteShader(shader);
        return 0;
    }

    bool ensureGlResourcesLocked() {
        if (g_state.program != 0 && g_state.captureTexture != 0) {
            return true;
        }

        static constexpr const char* kVertexSrc = R"(
            #version 300 es
            out vec2 vTex;
            void main() {
                vec2 pos;
                if (gl_VertexID == 0) {
                    pos = vec2(-1.0, -1.0);
                    vTex = vec2(0.0, 0.0);
                } else if (gl_VertexID == 1) {
                    pos = vec2(1.0, -1.0);
                    vTex = vec2(1.0, 0.0);
                } else if (gl_VertexID == 2) {
                    pos = vec2(-1.0, 1.0);
                    vTex = vec2(0.0, 1.0);
                } else {
                    pos = vec2(1.0, 1.0);
                    vTex = vec2(1.0, 1.0);
                }
                gl_Position = vec4(pos, 0.0, 1.0);
            }
        )";

        static constexpr const char* kFragmentSrc = R"(
            #version 300 es
            precision mediump float;
            in vec2 vTex;
            uniform sampler2D uTex;
            out vec4 fragColor;
            void main() {
                fragColor = texture(uTex, vTex);
            }
        )";

        g_state.vertexShader = compileShader(GL_VERTEX_SHADER, kVertexSrc);
        g_state.fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentSrc);
        if (g_state.vertexShader == 0 || g_state.fragmentShader == 0) {
            return false;
        }

        g_state.program = glCreateProgram();
        glAttachShader(g_state.program, g_state.vertexShader);
        glAttachShader(g_state.program, g_state.fragmentShader);
        glLinkProgram(g_state.program);

        GLint linkStatus = GL_FALSE;
        glGetProgramiv(g_state.program, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            char logBuffer[512] = {0};
            glGetProgramInfoLog(g_state.program, sizeof(logBuffer), nullptr, logBuffer);
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Program link failed: %s", logBuffer);
            return false;
        }

        glGenTextures(1, &g_state.captureTexture);
        glBindTexture(GL_TEXTURE_2D, g_state.captureTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        return true;
    }

    void destroyGlResourcesLocked() {
        if (g_state.captureTexture != 0) {
            glDeleteTextures(1, &g_state.captureTexture);
            g_state.captureTexture = 0;
        }
        if (g_state.program != 0) {
            glDeleteProgram(g_state.program);
            g_state.program = 0;
        }
        if (g_state.vertexShader != 0) {
            glDeleteShader(g_state.vertexShader);
            g_state.vertexShader = 0;
        }
        if (g_state.fragmentShader != 0) {
            glDeleteShader(g_state.fragmentShader);
            g_state.fragmentShader = 0;
        }
        g_state.captureWidth = 0;
        g_state.captureHeight = 0;
    }

    EGLConfig getConfigFromCurrentSurface(EGLDisplay display, EGLSurface surface) {
        EGLint configId = 0;
        if (eglQuerySurface(display, surface, EGL_CONFIG_ID, &configId) != EGL_TRUE || configId == 0) {
            return nullptr;
        }

        const EGLint attribs[] = {EGL_CONFIG_ID, configId, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint numConfigs = 0;
        if (eglChooseConfig(display, attribs, &config, 1, &numConfigs) != EGL_TRUE || numConfigs != 1) {
            return nullptr;
        }
        return config;
    }

    void destroyEncoderSurfaceLocked() {
        if (g_state.eglDisplay != EGL_NO_DISPLAY && g_state.eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(g_state.eglDisplay, g_state.eglSurface);
        }
        g_state.eglSurface = EGL_NO_SURFACE;
        g_state.eglDisplay = EGL_NO_DISPLAY;
        g_state.eglContext = EGL_NO_CONTEXT;

        if (g_state.window != nullptr) {
            ANativeWindow_release(g_state.window);
            g_state.window = nullptr;
        }

        g_state.surfaceDirty = false;
    }

    bool recreateEncoderSurfaceLocked(EGLDisplay display, EGLSurface currentDraw, EGLContext context) {
        if (g_state.window == nullptr) {
            destroyEncoderSurfaceLocked();
            return false;
        }

        if (g_state.eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(g_state.eglDisplay, g_state.eglSurface);
            g_state.eglSurface = EGL_NO_SURFACE;
        }

        EGLConfig config = getConfigFromCurrentSurface(display, currentDraw);
        if (config == nullptr) {
            logError("Failed to resolve EGLConfig from current surface");
            return false;
        }

        g_state.eglSurface = eglCreateWindowSurface(display, config, g_state.window, nullptr);
        if (g_state.eglSurface == EGL_NO_SURFACE) {
            logError("eglCreateWindowSurface failed for video encoder surface");
            return false;
        }

        g_state.eglDisplay = display;
        g_state.eglContext = context;
        g_state.surfaceDirty = false;

        if (g_state.presentationTimeProc == nullptr) {
            g_state.presentationTimeProc = reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(
                eglGetProcAddress("eglPresentationTimeANDROID")
            );
        }

        return true;
    }

    bool isEncoderSurfaceLocked(EGLDisplay display, EGLSurface surface) {
        return g_state.eglSurface != EGL_NO_SURFACE &&
            g_state.eglDisplay == display &&
            g_state.eglSurface == surface;
    }
}

namespace LinkuraLocal::StereoVideo {
    bool SetEncoderSurface(JNIEnv* env, jobject surface, int width, int height) {
        std::lock_guard<std::mutex> lock(g_state.mutex);

        if (g_state.window != nullptr) {
            ANativeWindow_release(g_state.window);
            g_state.window = nullptr;
        }

        if (surface != nullptr) {
            g_state.window = ANativeWindow_fromSurface(env, surface);
            if (g_state.window == nullptr) {
                logError("ANativeWindow_fromSurface returned null");
                g_state.targetWidth = 0;
                g_state.targetHeight = 0;
                g_state.surfaceDirty = true;
                return false;
            }
            g_state.targetWidth = width;
            g_state.targetHeight = height;
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "Encoder surface set: %dx%d", width, height);
        } else {
            g_state.targetWidth = 0;
            g_state.targetHeight = 0;
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "Encoder surface cleared");
        }

        g_state.surfaceDirty = true;
        return true;
    }

    void OnEndCameraRendering() {
        g_captureFrameReady.store(true, std::memory_order_release);
    }

    void OnEglSwapBuffers(EGLDisplay hookedDisplay, EGLSurface hookedSurface) {
        static uint64_t totalCalls = 0;
        static uint64_t submittedFrames = 0;
        static uint64_t skippedNoContext = 0;
        static uint64_t skippedNoTarget = 0;
        static uint64_t skippedNoFrame = 0;
        static uint64_t skippedRecreateFail = 0;
        static uint64_t skippedNoResources = 0;
        static uint64_t skippedBadSurfaceSize = 0;
        static uint64_t skippedMakeCurrentFail = 0;
        static auto lastStatsLogAt = std::chrono::steady_clock::now();
        totalCalls++;

        auto logPeriodic = [&]() {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatsLogAt).count();
            if (elapsedMs >= 2000) {
                __android_log_print(
                    ANDROID_LOG_INFO,
                    kLogTag,
                    "Bridge counters: calls=%llu submitted=%llu noContext=%llu noTarget=%llu noFrame=%llu recreateFail=%llu noResources=%llu badSurface=%llu makeCurrentFail=%llu",
                    static_cast<unsigned long long>(totalCalls),
                    static_cast<unsigned long long>(submittedFrames),
                    static_cast<unsigned long long>(skippedNoContext),
                    static_cast<unsigned long long>(skippedNoTarget),
                    static_cast<unsigned long long>(skippedNoFrame),
                    static_cast<unsigned long long>(skippedRecreateFail),
                    static_cast<unsigned long long>(skippedNoResources),
                    static_cast<unsigned long long>(skippedBadSurfaceSize),
                    static_cast<unsigned long long>(skippedMakeCurrentFail)
                );
                lastStatsLogAt = now;
            }
        };

        EGLDisplay display = eglGetCurrentDisplay();
        EGLContext context = eglGetCurrentContext();
        EGLSurface drawSurface = eglGetCurrentSurface(EGL_DRAW);
        EGLSurface readSurface = eglGetCurrentSurface(EGL_READ);

        if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT || drawSurface == EGL_NO_SURFACE || readSurface == EGL_NO_SURFACE) {
            skippedNoContext++;
            logPeriodic();
            return;
        }

        if (g_tlsInSwapHook) {
            return;
        }
        g_tlsInSwapHook = true;
        struct SwapHookScopeReset final {
            ~SwapHookScopeReset() {
                g_tlsInSwapHook = false;
            }
        } swapHookScopeReset;

        std::lock_guard<std::mutex> lock(g_state.mutex);

        if (isEncoderSurfaceLocked(hookedDisplay, hookedSurface) || isEncoderSurfaceLocked(display, drawSurface)) {
            return;
        }

        if (g_state.window == nullptr || g_state.targetWidth <= 0 || g_state.targetHeight <= 0) {
            skippedNoTarget++;
            logPeriodic();
            return;
        }

        const bool captureReady = g_captureFrameReady.exchange(false, std::memory_order_acq_rel);
        if (!captureReady) {
            skippedNoFrame++;
            logPeriodic();
            return;
        }

        const bool contextChanged = (g_state.eglDisplay != display) || (g_state.eglContext != context);
        if (g_state.surfaceDirty || contextChanged || g_state.eglSurface == EGL_NO_SURFACE) {
            destroyGlResourcesLocked();
            if (!recreateEncoderSurfaceLocked(display, drawSurface, context)) {
                skippedRecreateFail++;
                logPeriodic();
                return;
            }
        }

        if (!ensureGlResourcesLocked()) {
            skippedNoResources++;
            logPeriodic();
            return;
        }

        EGLint drawWidth = 0;
        EGLint drawHeight = 0;
        if (eglQuerySurface(display, drawSurface, EGL_WIDTH, &drawWidth) != EGL_TRUE ||
            eglQuerySurface(display, drawSurface, EGL_HEIGHT, &drawHeight) != EGL_TRUE ||
            drawWidth <= 0 || drawHeight <= 0) {
            skippedBadSurfaceSize++;
            logPeriodic();
            return;
        }

        if (g_state.captureWidth != drawWidth || g_state.captureHeight != drawHeight) {
            glBindTexture(GL_TEXTURE_2D, g_state.captureTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, drawWidth, drawHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
            g_state.captureWidth = drawWidth;
            g_state.captureHeight = drawHeight;
        }

        GLint prevProgram = 0;
        GLint prevActiveTex = 0;
        GLint prevTex2D = 0;
        GLint prevDrawFbo = 0;
        GLint prevReadFbo = 0;
        GLint prevViewport[4] = {0};
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
        glGetIntegerv(GL_VIEWPORT, prevViewport);

        auto restoreGlState = [&]() {
            glUseProgram(prevProgram);
            glActiveTexture(static_cast<GLenum>(prevActiveTex));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex2D));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFbo));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFbo));
            glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        };

        glBindTexture(GL_TEXTURE_2D, g_state.captureTexture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, drawWidth, drawHeight);

        if (eglMakeCurrent(display, g_state.eglSurface, g_state.eglSurface, context) != EGL_TRUE) {
            logError("eglMakeCurrent to encoder surface failed");
            skippedMakeCurrentFail++;
            g_captureFrameReady.store(true, std::memory_order_release);
            restoreGlState();
            logPeriodic();
            return;
        }

        glViewport(0, 0, g_state.targetWidth, g_state.targetHeight);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glUseProgram(g_state.program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_state.captureTexture);
        const GLint texLoc = glGetUniformLocation(g_state.program, "uTex");
        glUniform1i(texLoc, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (g_state.presentationTimeProc != nullptr) {
            const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();
            g_state.presentationTimeProc(display, g_state.eglSurface, static_cast<EGLnsecsANDROID>(nowNs));
        }

        eglSwapBuffers(display, g_state.eglSurface);
        submittedFrames++;
        logPeriodic();

        const EGLBoolean restoreResult = eglMakeCurrent(display, drawSurface, readSurface, context);
        if (restoreResult != EGL_TRUE) {
            logError("eglMakeCurrent restore failed");
            g_captureFrameReady.store(true, std::memory_order_release);
            return;
        }

        restoreGlState();
    }
}
