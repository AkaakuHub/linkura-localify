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
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import java.util.regex.Pattern

data class WebRtcStreamingConfig(
    val captureWidth: Int,
    val captureHeight: Int,
    val captureFps: Int,
    val minBitrateKbps: Int,
    val startBitrateKbps: Int,
    val maxBitrateKbps: Int,
    val degradationPreference: Int,
    val scaleResolutionDownBy: Double
)

class WebRtcSessionManager private constructor() {
    companion object {
        private const val TAG = "WebRtcSessionManager"
        private const val DEFAULT_SIGNALING_PORT = 39200
        private const val INPUT_FLAG_RESET_BASELINE = 1 shl 1
        private const val DEFAULT_CAPTURE_WIDTH = 1920
        private const val DEFAULT_CAPTURE_HEIGHT = 1080
        private const val DEFAULT_CAPTURE_FPS = 60
        private const val VIDEO_TRACK_ID = "bridge-video-track"
        private const val INPUT_DATA_CHANNEL_LABEL = "input-state"
        private const val INPUT_PAYLOAD_SIZE = 108
        private const val INPUT_TIMEOUT_MS = 300L
        private const val DEFAULT_MIN_BITRATE_KBPS = 6000
        private const val DEFAULT_START_BITRATE_KBPS = 14000
        private const val DEFAULT_MAX_BITRATE_KBPS = 28000
        private const val DEFAULT_DEGRADATION_PREFERENCE = 0
        private const val DEFAULT_SCALE_RESOLUTION_DOWN_BY = 1.0

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
    @Volatile
    private var signalingPort: Int = DEFAULT_SIGNALING_PORT
    private var factory: PeerConnectionFactory? = null
    private var peerConnection: PeerConnection? = null
    private var factoryEglBase: EglBase? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var videoCapturer: VideoCapturer? = null
    private var surfaceTextureHelper: SurfaceTextureHelper? = null
    private var eglBase: EglBase? = null
    private var appContext: Context? = null
    private var inputWatchdogExecutor: ScheduledExecutorService? = null
    private var inputDataChannel: DataChannel? = null
    private var lastInputPacketAtMs: Long = 0L
    private var zeroInputApplied = true
    @Volatile
    private var resetInputBaselineOnNextPacket = false
    private val peerLifecycleLock = Any()
    @Volatile
    private var streamingConfig = defaultStreamingConfig()
    private var videoSender: org.webrtc.RtpSender? = null

    fun start(context: Context) {
        if (!started.compareAndSet(false, true)) {
            return
        }
        appContext = context.applicationContext
        signalingClient.setPort(signalingPort)
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
                Log.d(TAG, "Signaling connected callback: rebuilding peer connection and creating fresh offer")
                rebuildPeerConnectionForReconnect()
                createOffer()
            } catch (e: Exception) {
                Log.e(TAG, "Signaling connected callback failed", e)
            }
        }
        signalingClient.start()
        startInputWatchdog()
    }

    fun setSignalingPort(port: Int) {
        val sanitizedPort = if (port in 1..65535) {
            port
        } else {
            DEFAULT_SIGNALING_PORT
        }

        if (signalingPort == sanitizedPort) {
            return
        }

        signalingPort = sanitizedPort
        signalingClient.setPort(sanitizedPort)
        if (started.get()) {
            signalingClient.restart()
        }
    }

    fun setStreamingConfig(config: WebRtcStreamingConfig) {
        val sanitizedConfig = sanitizeStreamingConfig(config)
        val previousConfig = streamingConfig
        if (previousConfig == sanitizedConfig) {
            return
        }

        streamingConfig = sanitizedConfig
        if (!started.get()) {
            return
        }

        val captureChanged = previousConfig.captureWidth != sanitizedConfig.captureWidth
            || previousConfig.captureHeight != sanitizedConfig.captureHeight
            || previousConfig.captureFps != sanitizedConfig.captureFps

        val shouldCreateOffer: Boolean
        synchronized(peerLifecycleLock) {
            if (captureChanged) {
                rebuildPeerConnectionLocked()
            } else {
                videoSender?.let { applyVideoSenderParameters(it) }
            }
            shouldCreateOffer = signalingClient.isConnected()
        }
        if (shouldCreateOffer) {
            createOffer()
        }
    }

    fun stop() {
        if (!started.compareAndSet(true, false)) {
            return
        }
        signalingClient.stop()
        synchronized(peerLifecycleLock) {
            teardownPeerConnectionLocked()
        }
        factory?.dispose()
        factory = null
        try {
            factoryEglBase?.release()
        } catch (_: Exception) {
        }
        factoryEglBase = null
        appContext = null
        stopInputWatchdog()
        applyZeroInput()
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
        Log.d(TAG, "PeerConnectionFactory initialized")
    }

    private fun buildPeerConnection() {
        synchronized(peerLifecycleLock) {
            buildPeerConnectionLocked()
        }
    }

    private fun buildPeerConnectionLocked() {
        val activeFactory = factory ?: return
        val iceServer = PeerConnection.IceServer.builder("stun:stun.l.google.com:19302").createIceServer()
        val config = PeerConnection.RTCConfiguration(listOf(iceServer))
        peerConnection = activeFactory.createPeerConnection(config, object : PeerConnection.Observer {
            override fun onSignalingChange(newState: PeerConnection.SignalingState) {
                Log.d(TAG, "Signaling state: $newState")
            }

            override fun onIceConnectionChange(newState: PeerConnection.IceConnectionState) {
                Log.d(TAG, "ICE connection state: $newState")
            }

            override fun onIceConnectionReceivingChange(receiving: Boolean) {
            }

            override fun onIceGatheringChange(newState: PeerConnection.IceGatheringState) {
                Log.d(TAG, "ICE gathering state: $newState")
            }

            override fun onIceCandidate(candidate: IceCandidate) {
                Log.d(
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
                Log.d(TAG, "Remote data channel opened: ${channel.label()}")
                if (channel.label() == INPUT_DATA_CHANNEL_LABEL) {
                    attachInputDataChannel(channel)
                }
            }

            override fun onRenegotiationNeeded() {
            }

            override fun onAddTrack(receiver: org.webrtc.RtpReceiver, streams: Array<out org.webrtc.MediaStream>) {
            }
        })

        val activePeerConnection = peerConnection ?: return
        createInputDataChannel(activePeerConnection)
        val context = appContext ?: return
        startVideoTrack(context, activeFactory, activePeerConnection)
    }

    private fun rebuildPeerConnectionForReconnect() {
        synchronized(peerLifecycleLock) {
            if (!started.get()) {
                return
            }
            teardownPeerConnectionLocked()
            buildPeerConnectionLocked()
        }
    }

    private fun teardownPeerConnectionLocked() {
        stopVideoTrack()
        clearInputDataChannel()
        try {
            peerConnection?.close()
        } catch (_: Exception) {
        }
        try {
            peerConnection?.dispose()
        } catch (_: Exception) {
        }
        peerConnection = null
        videoSender = null
    }

    private fun createInputDataChannel(activePeerConnection: PeerConnection) {
        clearInputDataChannel()
        val init = DataChannel.Init().apply {
            ordered = false
            maxRetransmits = 0
        }
        val channel = activePeerConnection.createDataChannel(INPUT_DATA_CHANNEL_LABEL, init)
        if (channel == null) {
            Log.w(TAG, "Failed to create input data channel")
            return
        }
        attachInputDataChannel(channel)
    }

    private fun attachInputDataChannel(channel: DataChannel) {
        clearInputDataChannel()
        inputDataChannel = channel
        channel.registerObserver(object : DataChannel.Observer {
            override fun onBufferedAmountChange(previousAmount: Long) {
            }

            override fun onStateChange() {
                val state = channel.state()
                Log.d(TAG, "Input data channel state: label=${channel.label()} state=$state")
                if (state == DataChannel.State.OPEN) {
                    lastInputPacketAtMs = System.currentTimeMillis()
                    resetInputBaselineOnNextPacket = true
                } else if (state == DataChannel.State.CLOSING || state == DataChannel.State.CLOSED) {
                    applyZeroInput()
                }
            }

            override fun onMessage(buffer: DataChannel.Buffer) {
                if (!buffer.binary) {
                    return
                }
                val payloadBuffer = buffer.data
                val payload = ByteArray(payloadBuffer.remaining())
                payloadBuffer.get(payload)
                handleInputPayload(payload)
            }
        })
    }

    private fun clearInputDataChannel() {
        val channel = inputDataChannel ?: return
        try {
            channel.unregisterObserver()
        } catch (_: Exception) {
        }
        try {
            channel.close()
        } catch (_: Exception) {
        }
        try {
            channel.dispose()
        } catch (_: Exception) {
        }
        inputDataChannel = null
    }

    private fun handleInputPayload(payload: ByteArray) {
        if (payload.size != INPUT_PAYLOAD_SIZE) {
            return
        }
        val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)

        val leftStickX = bb.float
        val leftStickY = bb.float
        val rightStickX = bb.float
        val rightStickY = bb.float
        val leftTrigger = bb.float
        val leftGrip = bb.float
        val rightTrigger = bb.float
        val rightGrip = bb.float
        val orientationX = bb.float
        val orientationY = bb.float
        val orientationZ = bb.float
        val orientationW = bb.float
        val hmdPosX = bb.float
        val hmdPosY = bb.float
        val hmdPosZ = bb.float
        val buttons = bb.int
        var flags = bb.int
        // Reset the native baseline exactly once after the data channel comes back.
        if (resetInputBaselineOnNextPacket) {
            flags = flags or INPUT_FLAG_RESET_BASELINE
            resetInputBaselineOnNextPacket = false
        }
        val ipdMeters = bb.float
        val hmdVerticalFovDegrees = bb.float
        val leftEyeAngleLeftRadians = bb.float
        val leftEyeAngleRightRadians = bb.float
        val leftEyeAngleUpRadians = bb.float
        val leftEyeAngleDownRadians = bb.float
        val rightEyeAngleLeftRadians = bb.float
        val rightEyeAngleRightRadians = bb.float
        val rightEyeAngleUpRadians = bb.float
        val rightEyeAngleDownRadians = bb.float

        val safeIpdMeters = ipdMeters.coerceIn(0.0f, 0.12f)
        val safeHmdVerticalFovDegrees = hmdVerticalFovDegrees.coerceIn(20.0f, 170.0f)
        val minFovAngle = -1.55f
        val maxFovAngle = 1.55f

        LinkuraHookMain.applyWindowsCameraInput(
            leftStickX,
            leftStickY,
            rightStickX,
            rightStickY,
            leftTrigger,
            leftGrip,
            rightTrigger,
            rightGrip,
            orientationX,
            orientationY,
            orientationZ,
            orientationW,
            hmdPosX,
            hmdPosY,
            hmdPosZ,
            buttons,
            flags,
            safeIpdMeters,
            safeHmdVerticalFovDegrees,
            leftEyeAngleLeftRadians.coerceIn(minFovAngle, maxFovAngle),
            leftEyeAngleRightRadians.coerceIn(minFovAngle, maxFovAngle),
            leftEyeAngleUpRadians.coerceIn(minFovAngle, maxFovAngle),
            leftEyeAngleDownRadians.coerceIn(minFovAngle, maxFovAngle),
            rightEyeAngleLeftRadians.coerceIn(minFovAngle, maxFovAngle),
            rightEyeAngleRightRadians.coerceIn(minFovAngle, maxFovAngle),
            rightEyeAngleUpRadians.coerceIn(minFovAngle, maxFovAngle),
            rightEyeAngleDownRadians.coerceIn(minFovAngle, maxFovAngle)
        )

        zeroInputApplied = false
        lastInputPacketAtMs = System.currentTimeMillis()
    }

    private fun startInputWatchdog() {
        stopInputWatchdog()
        lastInputPacketAtMs = System.currentTimeMillis()
        inputWatchdogExecutor = Executors.newSingleThreadScheduledExecutor { runnable ->
            Thread(runnable, "WebRtcInputWatchdog").apply { isDaemon = true }
        }.also { executor ->
            executor.scheduleAtFixedRate(
                {
                    try {
                        val now = System.currentTimeMillis()
                        if (now - lastInputPacketAtMs > INPUT_TIMEOUT_MS) {
                            applyZeroInput()
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "Input watchdog failed: ${e.message}")
                    }
                },
                50,
                50,
                TimeUnit.MILLISECONDS
            )
        }
    }

    private fun stopInputWatchdog() {
        val executor = inputWatchdogExecutor ?: return
        executor.shutdownNow()
        inputWatchdogExecutor = null
    }

    private fun applyZeroInput() {
        if (zeroInputApplied) {
            return
        }
        LinkuraHookMain.applyWindowsCameraInput(
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            1.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0,
            0,
            0.064f,
            90.0f,
            -0.7853982f,
            0.7853982f,
            0.7853982f,
            -0.7853982f,
            -0.7853982f,
            0.7853982f,
            0.7853982f,
            -0.7853982f
        )
        zeroInputApplied = true
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
        val activeStreamingConfig = streamingConfig
        capturer.startCapture(
            activeStreamingConfig.captureWidth,
            activeStreamingConfig.captureHeight,
            activeStreamingConfig.captureFps
        )
        val track = activeFactory.createVideoTrack(VIDEO_TRACK_ID, source)
        videoTrack = track
        val sender = activePeerConnection.addTrack(track)
        videoSender = sender
        applyVideoCodecPreference(activePeerConnection)
        applyVideoSenderParameters(sender)
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
        videoSender = null

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
                val tunedSdp = tuneVp8OfferSdp(description.description)
                Log.d(TAG, "Offer SDP summary: ${summarizeSdp(tunedSdp)}")
                val tunedDescription = SessionDescription(description.type, tunedSdp)
                activePeerConnection.setLocalDescription(
                    LoggingSdpObserver("setLocalDescription(offer)"),
                    tunedDescription
                )
                signalingClient.send(
                    WebRtcSignalingMessage(
                        type = "offer",
                        sdpType = tunedDescription.type.canonicalForm(),
                        sdp = tunedDescription.description
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
                Log.d(TAG, "Answer SDP summary: ${summarizeSdp(answerSdp)}")
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
                Log.d(TAG, "Remote ICE candidate added: mid=$mid mline=$index candidate=$rewrittenSdp")
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

    private fun applyVideoSenderParameters(sender: org.webrtc.RtpSender) {
        try {
            val parameters = sender.parameters ?: return
            val encodings = parameters.encodings
            if (encodings == null || encodings.isEmpty()) {
                Log.w(TAG, "Sender parameters skipped: encoding list is empty")
                return
            }
            val activeStreamingConfig = streamingConfig
            for (encoding in encodings) {
                encoding.maxBitrateBps = activeStreamingConfig.maxBitrateKbps * 1000
                encoding.minBitrateBps = activeStreamingConfig.minBitrateKbps * 1000
                encoding.maxFramerate = activeStreamingConfig.captureFps
                encoding.scaleResolutionDownBy = activeStreamingConfig.scaleResolutionDownBy
            }
            parameters.degradationPreference = mapDegradationPreference(activeStreamingConfig.degradationPreference)
            if (!sender.setParameters(parameters)) {
                Log.w(TAG, "Failed to apply sender bitrate parameters")
                return
            }
        } catch (e: Exception) {
            Log.e(TAG, "applyVideoSenderParameters failed", e)
        }
    }

    private fun tuneVp8OfferSdp(sdp: String): String {
        val lines = sdp.split("\r\n", "\n")
            .map { it.trim() }
            .filter { it.isNotEmpty() }
            .toMutableList()
        val vp8PayloadType = lines.firstOrNull { line ->
            line.startsWith("a=rtpmap:") && line.contains(" VP8/90000", ignoreCase = true)
        }?.substringAfter("a=rtpmap:")
            ?.substringBefore(" ")
            ?.trim()
            ?: return sdp

        val activeStreamingConfig = streamingConfig
        val fmtpLine =
            "a=fmtp:$vp8PayloadType x-google-min-bitrate=${activeStreamingConfig.minBitrateKbps};x-google-start-bitrate=${activeStreamingConfig.startBitrateKbps};x-google-max-bitrate=${activeStreamingConfig.maxBitrateKbps}"
        val nackLine = "a=rtcp-fb:$vp8PayloadType nack"
        val pliLine = "a=rtcp-fb:$vp8PayloadType nack pli"

        var hasFmtp = false
        var hasNack = false
        var hasPli = false
        for (index in lines.indices) {
            val line = lines[index]
            if (line.startsWith("a=fmtp:$vp8PayloadType")) {
                lines[index] = fmtpLine
                hasFmtp = true
            } else if (line == nackLine) {
                hasNack = true
            } else if (line == pliLine) {
                hasPli = true
            }
        }
        if (!hasFmtp) {
            val rtpmapIndex = lines.indexOfFirst { line ->
                line.startsWith("a=rtpmap:$vp8PayloadType ")
            }
            if (rtpmapIndex >= 0) {
                lines.add(rtpmapIndex + 1, fmtpLine)
            } else {
                lines.add(fmtpLine)
            }
        }
        if (!hasNack) {
            lines.add(nackLine)
        }
        if (!hasPli) {
            lines.add(pliLine)
        }
        return lines.joinToString(separator = "\r\n", postfix = "\r\n")
    }

    private fun applyVideoCodecPreference(activePeerConnection: PeerConnection) {
        val activeFactory = factory ?: run {
            Log.w(TAG, "Codec preference skipped: factory is null")
            return
        }
        val capabilities = activeFactory.getRtpSenderCapabilities(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO)
        val codecs: List<RtpCapabilities.CodecCapability> = capabilities.codecs ?: emptyList()
        val codecNames = codecs.joinToString(",") { codec -> "${codec.name}/${codec.kind}" }
        Log.d(TAG, "Sender video codec capabilities: $codecNames")

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
        Log.d(TAG, "Video transceiver direction: ${transceiver.direction}")
        Log.d(TAG, "Applied codec preference: VP8 family only (${orderedCodecs.size} entries)")
    }

    private class LoggingSdpObserver(private val action: String) : SdpObserver {
        override fun onCreateSuccess(description: SessionDescription) {
        }

        override fun onSetSuccess() {
            Log.d(TAG, "$action success")
        }

        override fun onCreateFailure(error: String) {
            Log.e(TAG, "$action create failure: $error")
        }

        override fun onSetFailure(error: String) {
            Log.e(TAG, "$action set failure: $error")
        }
    }

    private fun rebuildPeerConnectionLocked() {
        teardownPeerConnectionLocked()
        buildPeerConnectionLocked()
    }

    private fun defaultStreamingConfig(): WebRtcStreamingConfig {
        return WebRtcStreamingConfig(
            captureWidth = DEFAULT_CAPTURE_WIDTH,
            captureHeight = DEFAULT_CAPTURE_HEIGHT,
            captureFps = DEFAULT_CAPTURE_FPS,
            minBitrateKbps = DEFAULT_MIN_BITRATE_KBPS,
            startBitrateKbps = DEFAULT_START_BITRATE_KBPS,
            maxBitrateKbps = DEFAULT_MAX_BITRATE_KBPS,
            degradationPreference = DEFAULT_DEGRADATION_PREFERENCE,
            scaleResolutionDownBy = DEFAULT_SCALE_RESOLUTION_DOWN_BY
        )
    }

    private fun sanitizeStreamingConfig(config: WebRtcStreamingConfig): WebRtcStreamingConfig {
        val captureWidth = sanitizeCaptureDimension(config.captureWidth, DEFAULT_CAPTURE_WIDTH)
        val captureHeight = sanitizeCaptureDimension(config.captureHeight, DEFAULT_CAPTURE_HEIGHT)
        val captureFps = if (config.captureFps in 1..120) config.captureFps else DEFAULT_CAPTURE_FPS
        val minBitrateKbps = sanitizeBitrate(config.minBitrateKbps, DEFAULT_MIN_BITRATE_KBPS)
        val startBitrateKbps = sanitizeBitrate(config.startBitrateKbps, DEFAULT_START_BITRATE_KBPS)
        val maxBitrateKbps = sanitizeBitrate(config.maxBitrateKbps, DEFAULT_MAX_BITRATE_KBPS)
        val normalizedMinBitrateKbps = minBitrateKbps.coerceAtMost(startBitrateKbps).coerceAtMost(maxBitrateKbps)
        val normalizedStartBitrateKbps = startBitrateKbps.coerceAtLeast(normalizedMinBitrateKbps).coerceAtMost(maxBitrateKbps)
        val normalizedMaxBitrateKbps = maxBitrateKbps.coerceAtLeast(normalizedStartBitrateKbps)
        val degradationPreference = if (config.degradationPreference in 0..3) {
            config.degradationPreference
        } else {
            DEFAULT_DEGRADATION_PREFERENCE
        }
        val scaleResolutionDownBy = if (config.scaleResolutionDownBy in 1.0..4.0) {
            config.scaleResolutionDownBy
        } else {
            DEFAULT_SCALE_RESOLUTION_DOWN_BY
        }

        return WebRtcStreamingConfig(
            captureWidth = captureWidth,
            captureHeight = captureHeight,
            captureFps = captureFps,
            minBitrateKbps = normalizedMinBitrateKbps,
            startBitrateKbps = normalizedStartBitrateKbps,
            maxBitrateKbps = normalizedMaxBitrateKbps,
            degradationPreference = degradationPreference,
            scaleResolutionDownBy = scaleResolutionDownBy
        )
    }

    private fun sanitizeCaptureDimension(value: Int, defaultValue: Int): Int {
        val sanitized = if (value in 256..7680) value else defaultValue
        return if (sanitized % 2 == 0) sanitized else sanitized - 1
    }

    private fun sanitizeBitrate(value: Int, defaultValue: Int): Int {
        return if (value in 100..200000) value else defaultValue
    }

    private fun mapDegradationPreference(value: Int): org.webrtc.RtpParameters.DegradationPreference {
        return when (value) {
            1 -> org.webrtc.RtpParameters.DegradationPreference.MAINTAIN_FRAMERATE
            2 -> org.webrtc.RtpParameters.DegradationPreference.MAINTAIN_RESOLUTION
            3 -> org.webrtc.RtpParameters.DegradationPreference.BALANCED
            else -> org.webrtc.RtpParameters.DegradationPreference.DISABLED
        }
    }
}
