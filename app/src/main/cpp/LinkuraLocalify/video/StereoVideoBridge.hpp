#pragma once

#include <jni.h>
#include <EGL/egl.h>

namespace LinkuraLocal::StereoVideo {
    bool SetEncoderSurface(JNIEnv* env, jobject surface, int width, int height);
    void OnEndCameraRendering();
    void OnEglSwapBuffers(EGLDisplay display, EGLSurface surface);
}
