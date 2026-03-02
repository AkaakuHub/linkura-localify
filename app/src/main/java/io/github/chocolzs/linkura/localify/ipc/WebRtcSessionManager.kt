package io.github.chocolzs.linkura.localify.ipc

import android.content.Context
import android.util.Log
import io.github.chocolzs.linkura.localify.LinkuraHookMain
import org.webrtc.DataChannel
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpCapabilities
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoCapturer
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import java.util.regex.Pattern

class WebRtcSessionManager private constructor() {
    companion object {
        private const val TAG = "WebRtcSessionManager"
        private const val CAPTURE_WIDTH = 1920
        private const val CAPTURE_HEIGHT = 1080
        private const val CAPTURE_FPS = 60
        private const val VIDEO_TRACK_ID = "bridge-video-track"

        @Volatile
        private var INSTANCE: WebRtcSessionManager? = null

        fun getInstance(): WebRtcSessionManager {
            return INSTANCE ?: synchronized(this) {
                INSTANCE ?: WebRtcSessionManager().also { INSTANCE = it }
            }
        }
    }

    private val signalingClient = WebRtcSignalingTcpClient()
    private val started = AtomicBoolean(false)
    private var factory: PeerConnectionFactory? = null
    private var peerConnection: PeerConnection? = null
    private var factoryEglBase: EglBase? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var videoCapturer: VideoCapturer? = null
    private var surfaceTextureHelper: SurfaceTextureHelper? = null
    private var eglBase: EglBase? = null
    private var appContext: Context? = null
    private var statsExecutor: ScheduledExecutorService? = null

    fun start(context: Context) {
        if (!started.compareAndSet(false, true)) {
            return
        }
        appContext = context.applicationContext
        ensureFactory(context)
        buildPeerConnection()
        signalingClient.setOnMessageListener { message ->
            try {
                handleSignalingMessage(message)
            } catch (e: Exception) {
                Log.e(TAG, "Signaling message handling failed", e)
            }
        }
        signalingClient.setOnConnectedListener {
            try {
                Log.i(TAG, "Signaling connected callback: creating fresh offer")
                createOffer()
            } catch (e: Exception) {
                Log.e(TAG, "Signaling connected callback failed", e)
            }
        }
        signalingClient.start()
        startStatsLogging()
    }

    fun stop() {
        if (!started.compareAndSet(true, false)) {
            return
        }
        signalingClient.stop()
        stopVideoTrack()
        peerConnection?.close()
        peerConnection?.dispose()
        peerConnection = null
        factory?.dispose()
        factory = null
        try {
            factoryEglBase?.release()
        } catch (_: Exception) {
        }
        factoryEglBase = null
        appContext = null
        stopStatsLogging()
        try {
            LinkuraHookMain.setVideoEncoderSurface(null, 0, 0)
        } catch (_: Exception) {
        }
    }

    private fun ensureFactory(context: Context) {
        if (factory != null) {
            return
        }
        PeerConnectionFactory.initialize(
            PeerConnectionFactory.InitializationOptions.builder(context).createInitializationOptions()
        )
        val egl = EglBase.create()
        factoryEglBase = egl
        val eglContext = egl.eglBaseContext
        val encoderFactory = DefaultVideoEncoderFactory(eglContext, true, true)
        val decoderFactory = DefaultVideoDecoderFactory(eglContext)
        factory = PeerConnectionFactory.builder()
            .setVideoEncoderFactory(encoderFactory)
            .setVideoDecoderFactory(decoderFactory)
            .createPeerConnectionFactory()
        Log.i(TAG, "PeerConnectionFactory initialized")
    }

    private fun buildPeerConnection() {
        val activeFactory = factory ?: return
        val iceServer = PeerConnection.IceServer.builder("stun:stun.l.google.com:19302").createIceServer()
        val config = PeerConnection.RTCConfiguration(listOf(iceServer))
        peerConnection = activeFactory.createPeerConnection(config, object : PeerConnection.Observer {
            override fun onSignalingChange(newState: PeerConnection.SignalingState) {
                Log.i(TAG, "Signaling state: $newState")
            }

            override fun onIceConnectionChange(newState: PeerConnection.IceConnectionState) {
                Log.i(TAG, "ICE connection state: $newState")
            }

            override fun onIceConnectionReceivingChange(receiving: Boolean) {
            }

            override fun onIceGatheringChange(newState: PeerConnection.IceGatheringState) {
                Log.i(TAG, "ICE gathering state: $newState")
            }

            override fun onIceCandidate(candidate: IceCandidate) {
                Log.i(
                    TAG,
                    "Local ICE candidate: mid=${candidate.sdpMid} mline=${candidate.sdpMLineIndex} candidate=${candidate.sdp}"
                )
                signalingClient.send(
                    WebRtcSignalingMessage(
                        type = "ice-candidate",
                        candidate = candidate.sdp,
                        sdpMid = candidate.sdpMid,
                        sdpMLineIndex = candidate.sdpMLineIndex
                    )
                )
            }

            override fun onIceCandidatesRemoved(candidates: Array<out IceCandidate>) {
            }

            override fun onAddStream(stream: org.webrtc.MediaStream) {
            }

            override fun onRemoveStream(stream: org.webrtc.MediaStream) {
            }

            override fun onDataChannel(channel: DataChannel) {
                Log.i(TAG, "Remote data channel opened: ${channel.label()}")
            }

            override fun onRenegotiationNeeded() {
            }

            override fun onAddTrack(receiver: org.webrtc.RtpReceiver, streams: Array<out org.webrtc.MediaStream>) {
            }
        })

        val activePeerConnection = peerConnection ?: return
        val context = appContext ?: return
        startVideoTrack(context, activeFactory, activePeerConnection)
    }

    private fun startVideoTrack(
        context: Context,
        activeFactory: PeerConnectionFactory,
        activePeerConnection: PeerConnection
    ) {
        stopVideoTrack()
        eglBase = EglBase.create()
        val eglContext = eglBase?.eglBaseContext ?: return
        val helper = SurfaceTextureHelper.create("BridgeVideoCapture", eglContext)
        surfaceTextureHelper = helper
        val source = activeFactory.createVideoSource(false)
        videoSource = source
        val capturer = BridgeSurfaceVideoCapturer { surface, width, height ->
            try {
                LinkuraHookMain.setVideoEncoderSurface(surface, width, height)
            } catch (e: Exception) {
                Log.e(TAG, "setVideoEncoderSurface failed", e)
            }
        }
        videoCapturer = capturer
        capturer.initialize(helper, context, source.capturerObserver)
        capturer.startCapture(CAPTURE_WIDTH, CAPTURE_HEIGHT, CAPTURE_FPS)
        val track = activeFactory.createVideoTrack(VIDEO_TRACK_ID, source)
        videoTrack = track
        val sender = activePeerConnection.addTrack(track)
        applyVideoCodecPreference(activePeerConnection)
        Log.i(TAG, "WebRTC video track started: ${CAPTURE_WIDTH}x${CAPTURE_HEIGHT}@${CAPTURE_FPS}")
        Log.i(TAG, "WebRTC video sender created: id=${sender.id()}")
    }

    private fun stopVideoTrack() {
        try {
            videoCapturer?.stopCapture()
        } catch (_: Exception) {
        }
        try {
            videoCapturer?.dispose()
        } catch (_: Exception) {
        }
        videoCapturer = null

        try {
            videoTrack?.setEnabled(false)
            videoTrack?.dispose()
        } catch (_: Exception) {
        }
        videoTrack = null

        try {
            videoSource?.dispose()
        } catch (_: Exception) {
        }
        videoSource = null

        try {
            surfaceTextureHelper?.dispose()
        } catch (_: Exception) {
        }
        surfaceTextureHelper = null

        try {
            eglBase?.release()
        } catch (_: Exception) {
        }
        eglBase = null
    }

    private fun createOffer() {
        val activePeerConnection = peerConnection ?: return
        activePeerConnection.createOffer(object : SdpObserver {
            override fun onCreateSuccess(description: SessionDescription) {
                Log.i(TAG, "Offer SDP summary: ${summarizeSdp(description.description)}")
                activePeerConnection.setLocalDescription(
                    LoggingSdpObserver("setLocalDescription(offer)"),
                    description
                )
                signalingClient.send(
                    WebRtcSignalingMessage(
                        type = "offer",
                        sdpType = description.type.canonicalForm(),
                        sdp = description.description
                    )
                )
            }

            override fun onSetSuccess() {
            }

            override fun onCreateFailure(error: String) {
                Log.e(TAG, "Failed to create offer: $error")
            }

            override fun onSetFailure(error: String) {
                Log.e(TAG, "Failed to set offer: $error")
            }
        }, MediaConstraints())
    }

    private fun handleSignalingMessage(message: WebRtcSignalingMessage) {
        val activePeerConnection = peerConnection ?: return
        when (message.type) {
            "answer" -> {
                val answerSdp = message.sdp ?: return
                Log.i(TAG, "Answer SDP summary: ${summarizeSdp(answerSdp)}")
                val remoteDescription = SessionDescription(SessionDescription.Type.ANSWER, answerSdp)
                activePeerConnection.setRemoteDescription(
                    LoggingSdpObserver("setRemoteDescription(answer)"),
                    remoteDescription
                )
            }

            "ice-candidate" -> {
                val sdp = message.candidate ?: return
                val mid = message.sdpMid
                val index = message.sdpMLineIndex ?: return
                if (mid.isNullOrBlank()) {
                    Log.w(TAG, "Ignore ICE candidate: missing sdpMid")
                    return
                }
                val rewrittenSdp = rewriteLoopbackCandidateForEmulator(sdp)
                Log.i(TAG, "Remote ICE candidate added: mid=$mid mline=$index candidate=$rewrittenSdp")
                activePeerConnection.addIceCandidate(IceCandidate(mid, index, rewrittenSdp))
            }
        }
    }

    private fun rewriteLoopbackCandidateForEmulator(candidateSdp: String): String {
        if (!isLikelyEmulator()) {
            return candidateSdp
        }
        var rewritten = candidateSdp
        rewritten = rewritten.replace(" 127.0.0.1 ", " 10.0.2.2 ")
        rewritten = rewritten.replace(" ::1 ", " 10.0.2.2 ")
        if (rewritten == candidateSdp) {
            val ipPattern = Pattern.compile("(candidate:\\S+\\s\\d+\\s(?:udp|tcp)\\s\\d+\\s)(\\S+)(\\s\\d+\\styp\\s\\S+.*)")
            val matcher = ipPattern.matcher(candidateSdp)
            if (matcher.matches()) {
                val ip = matcher.group(2) ?: ""
                if (ip == "127.0.0.1" || ip == "::1") {
                    val prefix = matcher.group(1) ?: ""
                    val suffix = matcher.group(3) ?: ""
                    rewritten = prefix + "10.0.2.2" + suffix
                }
            }
        }
        return rewritten
    }

    private fun isLikelyEmulator(): Boolean {
        return android.os.Build.FINGERPRINT.contains("generic")
            || android.os.Build.MODEL.contains("Emulator")
            || android.os.Build.HARDWARE.contains("ranchu")
            || android.os.Build.PRODUCT.contains("sdk")
    }

    private fun summarizeSdp(sdp: String): String {
        val lines = sdp.split("\r\n", "\n")
            .map { it.trim() }
            .filter { it.isNotEmpty() }
        val mLines = lines.filter { it.startsWith("m=") }.joinToString(" | ")
        val rtpMaps = lines.filter { it.startsWith("a=rtpmap:") }.joinToString(" | ")
        val bundle = lines.firstOrNull { it.startsWith("a=group:BUNDLE") } ?: ""
        return "len=${sdp.length}; bundle=$bundle; m=$mLines; rtpmap=$rtpMaps"
    }

    private fun startStatsLogging() {
        stopStatsLogging()
        val activePeerConnection = peerConnection ?: return
        statsExecutor = Executors.newSingleThreadScheduledExecutor { runnable ->
            Thread(runnable, "WebRtcStats").apply { isDaemon = true }
        }.also { executor ->
            executor.scheduleAtFixedRate(
                {
                    try {
                        activePeerConnection.getStats { report ->
                            logOutboundVideoStats(report.statsMap.values)
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "Stats polling failed: ${e.message}")
                    }
                },
                2,
                2,
                TimeUnit.SECONDS
            )
        }
    }

    private fun stopStatsLogging() {
        val executor = statsExecutor ?: return
        executor.shutdownNow()
        statsExecutor = null
    }

    private fun logOutboundVideoStats(stats: Collection<org.webrtc.RTCStats>) {
        val outboundVideo = stats.firstOrNull { stat ->
            stat.type == "outbound-rtp"
                && (
                    stat.members["kind"] == "video"
                        || stat.members["mediaType"] == "video"
                    )
        } ?: return

        val bytesSent = outboundVideo.members["bytesSent"] ?: "n/a"
        val packetsSent = outboundVideo.members["packetsSent"] ?: "n/a"
        val framesEncoded = outboundVideo.members["framesEncoded"] ?: "n/a"
        val framesSent = outboundVideo.members["framesSent"] ?: "n/a"
        val qpSum = outboundVideo.members["qpSum"] ?: "n/a"
        Log.i(
            TAG,
            "Outbound video stats: bytesSent=$bytesSent packetsSent=$packetsSent framesEncoded=$framesEncoded framesSent=$framesSent qpSum=$qpSum"
        )
    }

    private fun applyVideoCodecPreference(activePeerConnection: PeerConnection) {
        val activeFactory = factory ?: run {
            Log.w(TAG, "Codec preference skipped: factory is null")
            return
        }
        val capabilities = activeFactory.getRtpSenderCapabilities(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO)
        val codecs: List<RtpCapabilities.CodecCapability> = capabilities.codecs ?: emptyList()
        val codecNames = codecs.joinToString(",") { codec -> "${codec.name}/${codec.kind}" }
        Log.i(TAG, "Sender video codec capabilities: $codecNames")

        val vp8FamilyCodecs = codecs.filter { codec ->
            codec.kind == MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO
                && (
                    codec.name.equals("VP8", ignoreCase = true)
                        || codec.name.equals("RTX", ignoreCase = true)
                        || codec.name.equals("RED", ignoreCase = true)
                        || codec.name.equals("ULPFEC", ignoreCase = true)
                    )
        }
        if (vp8FamilyCodecs.isEmpty()) {
            throw IllegalStateException("VP8 codec capability not found. VP8 fixed mode cannot start.")
        }
        val orderedCodecs = ArrayList(vp8FamilyCodecs)

        val transceiver = activePeerConnection.transceivers.firstOrNull { transceiver ->
            transceiver.mediaType == MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO
        }
        if (transceiver == null) {
            throw IllegalStateException("Video transceiver not found. VP8 fixed mode cannot start.")
        }
        transceiver.setCodecPreferences(orderedCodecs)
        Log.i(TAG, "Video transceiver direction: ${transceiver.direction}")
        Log.i(TAG, "Applied codec preference: VP8 family only (${orderedCodecs.size} entries)")
    }

    private class LoggingSdpObserver(private val action: String) : SdpObserver {
        override fun onCreateSuccess(description: SessionDescription) {
        }

        override fun onSetSuccess() {
            Log.i(TAG, "$action success")
        }

        override fun onCreateFailure(error: String) {
            Log.e(TAG, "$action create failure: $error")
        }

        override fun onSetFailure(error: String) {
            Log.e(TAG, "$action set failure: $error")
        }
    }
}
