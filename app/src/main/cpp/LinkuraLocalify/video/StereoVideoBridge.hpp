#pragma once

#include <jni.h>

namespace LinkuraLocal::StereoVideo {
    bool SetEncoderSurface(JNIEnv* env, jobject surface, int width, int height);
    void OnEndCameraRendering();
}
