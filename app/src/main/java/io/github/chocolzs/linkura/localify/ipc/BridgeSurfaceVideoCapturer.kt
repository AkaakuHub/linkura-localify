package io.github.chocolzs.linkura.localify.ipc

import android.content.Context
import android.view.Surface
import org.webrtc.CapturerObserver
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoCapturer
import org.webrtc.VideoFrame
import android.util.Log

class BridgeSurfaceVideoCapturer(
    private val onSurfaceChanged: (Surface?, Int, Int) -> Unit
) : VideoCapturer {
    companion object {
        private const val TAG = "BridgeSurfaceCapturer"
    }

    private var textureHelper: SurfaceTextureHelper? = null
    private var observer: CapturerObserver? = null
    private var captureSurface: Surface? = null
    private var started = false
    private var width = 0
    private var height = 0
    private var capturedFrameCount = 0L

    override fun initialize(
        surfaceTextureHelper: SurfaceTextureHelper,
        applicationContext: Context,
        capturerObserver: CapturerObserver
    ) {
        textureHelper = surfaceTextureHelper
        observer = capturerObserver
    }

    override fun startCapture(width: Int, height: Int, framerate: Int) {
        val helper = textureHelper ?: return
        val activeObserver = observer ?: return
        if (started) {
            return
        }

        this.width = width
        this.height = height
        helper.setTextureSize(width, height)
        helper.setFrameRotation(0)
        captureSurface = Surface(helper.surfaceTexture)
        helper.startListening { frame: VideoFrame ->
            capturedFrameCount += 1
            activeObserver.onFrameCaptured(frame)
        }
        activeObserver.onCapturerStarted(true)
        started = true
        capturedFrameCount = 0
        onSurfaceChanged(captureSurface, width, height)
        Log.d(TAG, "startCapture ${width}x${height}@${framerate}")
    }

    override fun stopCapture() {
        if (!started) {
            return
        }
        started = false
        try {
            onSurfaceChanged(null, 0, 0)
        } catch (_: Exception) {
        }
        try {
            textureHelper?.stopListening()
        } catch (_: Exception) {
        }
        try {
            captureSurface?.release()
        } catch (_: Exception) {
        }
        captureSurface = null
        observer?.onCapturerStopped()
        Log.d(TAG, "stopCapture capturedFrameCount=$capturedFrameCount")
    }

    override fun changeCaptureFormat(width: Int, height: Int, framerate: Int) {
        this.width = width
        this.height = height
        textureHelper?.setTextureSize(width, height)
        onSurfaceChanged(captureSurface, width, height)
    }

    override fun dispose() {
        stopCapture()
    }

    override fun isScreencast(): Boolean {
        return false
    }
}
