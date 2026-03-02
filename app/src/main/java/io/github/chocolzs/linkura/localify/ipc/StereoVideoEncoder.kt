package io.github.chocolzs.linkura.localify.ipc

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.io.ByteArrayOutputStream
import java.util.Locale
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.ceil
class StereoVideoEncoder private constructor(
    private val videoClient: WindowsVideoTcpClient
) {
    companion object {
        private const val TAG = "StereoVideoEncoder"
        private const val MIME_TYPE = "video/avc"
        private const val MAX_BITRATE = 35_000_000
        private const val START_BITRATE = 24_000_000
        private const val MIN_BITRATE = 10_000_000
        private const val FRAME_RATE = 60
        private const val I_FRAME_INTERVAL_SECONDS = 1
        private const val STATS_WINDOW_MS = 1000L
    }

    private var codec: MediaCodec? = null
    private var callbackThread: HandlerThread? = null
    private var callbackHandler: Handler? = null
    private var started = false
    private var width = 0
    private var height = 0
    private var outputCount = 0L
    private var targetBitrate = START_BITRATE
    private var stableWindowCount = 0
    private var lastOverflowDropCount = 0L
    private val callbackInFlight = AtomicInteger(0)
    private val statsLock = Any()
    private var statsWindowStartMs = 0L
    private var statsEncodedFrames = 0
    private var statsEncodedBytes = 0L
    private val statsAuSizes = ArrayList<Int>()
    private var lastKeyFrameTimeMs = 0L
    private var iframeIntervalObservedSec = 1.0f

    var inputSurface: Surface? = null
        private set

    val encodeWidth: Int
        get() = width

    val encodeHeight: Int
        get() = height

    init {
        videoClient.setOnConnectedListener {
            requestKeyFrame()
        }
        videoClient.setOnSyncFrameRequestListener {
            requestKeyFrame()
        }
    }

    fun start(requestWidth: Int, requestHeight: Int): Boolean {
        if (started) {
            return true
        }

        if (requestWidth <= 0 || requestHeight <= 0) {
            Log.e(TAG, "Invalid encoder size: ${requestWidth}x${requestHeight}")
            return false
        }

        val codecInfo = findAvcEncoderWithSurface()
        if (codecInfo == null) {
            Log.e(TAG, "No AVC encoder with Surface input found")
            return false
        }

        val encodeSize = resolveEncodeSize(codecInfo, requestWidth, requestHeight)
        if (encodeSize == null) {
            Log.e(TAG, "Requested encoder size unsupported: ${requestWidth}x${requestHeight}, codec=${codecInfo.name}")
            return false
        }

        width = encodeSize.first
        height = encodeSize.second
        targetBitrate = START_BITRATE

        return try {
            val format = MediaFormat.createVideoFormat(MIME_TYPE, width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, targetBitrate)
                setInteger(MediaFormat.KEY_FRAME_RATE, FRAME_RATE)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, I_FRAME_INTERVAL_SECONDS)
                setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            }

            callbackThread = HandlerThread("StereoVideoEncoderThread").also { it.start() }
            callbackHandler = Handler(callbackThread!!.looper)
            synchronized(statsLock) {
                statsWindowStartMs = System.currentTimeMillis()
                statsEncodedFrames = 0
                statsEncodedBytes = 0L
                statsAuSizes.clear()
                lastKeyFrameTimeMs = 0L
                iframeIntervalObservedSec = I_FRAME_INTERVAL_SECONDS.toFloat()
                stableWindowCount = 0
                lastOverflowDropCount = videoClient.getDroppedFramesOverflowCount()
            }

            codec = MediaCodec.createByCodecName(codecInfo.name).also { mediaCodec ->
                mediaCodec.setCallback(createCodecCallback(), callbackHandler)
                mediaCodec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                inputSurface = mediaCodec.createInputSurface()
                mediaCodec.start()
            }

            started = true
            videoClient.startClient()
            Log.i(
                TAG,
                "Stereo video encoder started: ${width}x${height}, codec=${codecInfo.name}, targetBitrate=$targetBitrate, targetFps=$FRAME_RATE, iFrameIntervalSec=$I_FRAME_INTERVAL_SECONDS"
            )
            true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start video encoder", e)
            stop()
            false
        }
    }

    fun stop() {
        started = false

        try {
            codec?.stop()
        } catch (_: Exception) {
        }

        try {
            codec?.release()
        } catch (_: Exception) {
        }
        codec = null

        try {
            inputSurface?.release()
        } catch (_: Exception) {
        }
        inputSurface = null

        callbackThread?.quitSafely()
        callbackThread = null
        callbackHandler = null

        videoClient.stopClient()
    }

    fun requestKeyFrame() {
        val mediaCodec = codec ?: return
        try {
            val params = android.os.Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            }
            mediaCodec.setParameters(params)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to request key frame: ${e.message}")
        }
    }

    private fun createCodecCallback(): MediaCodec.Callback {
        return object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
            }

            override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                callbackInFlight.incrementAndGet()
                try {
                    if (info.size <= 0) {
                        codec.releaseOutputBuffer(index, false)
                        return
                    }

                    val outputBuffer = codec.getOutputBuffer(index)
                    if (outputBuffer == null) {
                        codec.releaseOutputBuffer(index, false)
                        return
                    }

                    outputBuffer.position(info.offset)
                    outputBuffer.limit(info.offset + info.size)
                    val accessUnit = ByteArray(info.size)
                    outputBuffer.get(accessUnit)
                    val normalizedAccessUnit = normalizeAccessUnitToAnnexB(accessUnit)
                    if (normalizedAccessUnit == null) {
                        Log.w(TAG, "Drop invalid AU: size=${info.size}, flags=${info.flags}")
                        codec.releaseOutputBuffer(index, false)
                        return
                    }

                    videoClient.enqueueAccessUnit(
                        payload = normalizedAccessUnit,
                        codecFlags = info.flags
                    )
                    recordStats(normalizedAccessUnit.size, info.flags)
                    outputCount++
                    if (outputCount <= 5L || outputCount % 120L == 0L) {
                        Log.i(
                            TAG,
                            "Encoder output: count=$outputCount rawSize=${info.size} normalizedSize=${normalizedAccessUnit.size} flags=${info.flags}"
                        )
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error handling output buffer", e)
                } finally {
                    callbackInFlight.decrementAndGet()
                    try {
                        codec.releaseOutputBuffer(index, false)
                    } catch (_: Exception) {
                    }
                }
            }

            override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                Log.i(TAG, "Output format changed: $format")
                val csd0 = format.getByteBuffer("csd-0")
                val csd1 = format.getByteBuffer("csd-1")
                if (csd0 == null || csd1 == null) {
                    Log.w(TAG, "Output format changed without SPS/PPS")
                    return
                }

                val sps = ByteArray(csd0.remaining())
                csd0.get(sps)
                val pps = ByteArray(csd1.remaining())
                csd1.get(pps)

                val annexBSps = toAnnexB(sps)
                val annexBPps = toAnnexB(pps)
                val configPayload = ByteArray(annexBSps.size + annexBPps.size)
                System.arraycopy(annexBSps, 0, configPayload, 0, annexBSps.size)
                System.arraycopy(annexBPps, 0, configPayload, annexBSps.size, annexBPps.size)

                videoClient.updateCodecConfig(configPayload)
                Log.i(TAG, "Output format ready. SPS=${sps.size} bytes, PPS=${pps.size} bytes")
                requestKeyFrame()
            }

            override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                Log.e(TAG, "MediaCodec error: ${e.diagnosticInfo}")
            }
        }
    }

    private fun recordStats(auSize: Int, codecFlags: Int) {
        synchronized(statsLock) {
            val nowMs = System.currentTimeMillis()
            if (statsWindowStartMs == 0L) {
                statsWindowStartMs = nowMs
            }

            statsEncodedFrames++
            statsEncodedBytes += auSize.toLong()
            statsAuSizes.add(auSize)

            val isKeyFrame = (codecFlags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
            if (isKeyFrame) {
                if (lastKeyFrameTimeMs > 0L) {
                    iframeIntervalObservedSec = ((nowMs - lastKeyFrameTimeMs).coerceAtLeast(1L) / 1000.0f)
                }
                lastKeyFrameTimeMs = nowMs
            }

            val elapsedMs = nowMs - statsWindowStartMs
            if (elapsedMs < STATS_WINDOW_MS) {
                return
            }

            val encFps = (statsEncodedFrames * 1000L / elapsedMs).toInt()
            val actualBitrate = if (elapsedMs > 0) (statsEncodedBytes * 8_000L / elapsedMs) else 0L
            val avgAuSize = if (statsAuSizes.isEmpty()) 0 else (statsAuSizes.sum().toLong() / statsAuSizes.size).toInt()
            val p95AuSize = computeP95(statsAuSizes)
            val tcpQueueDepth = videoClient.getQueueDepth()
            val encQueueDepth = callbackInFlight.get().coerceAtLeast(0)
            maybeAdjustBitrate(tcpQueueDepth)

            Log.i(
                TAG,
                "enc_fps=$encFps enc_bitrate_actual=$actualBitrate enc_target_bitrate=$targetBitrate enc_iframe_interval_sec=${String.format(Locale.US, "%.3f", iframeIntervalObservedSec)} enc_queue_depth=$encQueueDepth tcp_send_q_depth=$tcpQueueDepth au_size_avg_bytes=$avgAuSize au_size_p95_bytes=$p95AuSize"
            )

            statsWindowStartMs = nowMs
            statsEncodedFrames = 0
            statsEncodedBytes = 0L
            statsAuSizes.clear()
        }
    }

    private fun computeP95(values: List<Int>): Int {
        if (values.isEmpty()) {
            return 0
        }
        val sorted = values.sorted()
        val index = (ceil(sorted.size * 0.95) - 1.0).toInt().coerceIn(0, sorted.lastIndex)
        return sorted[index]
    }

    private fun maybeAdjustBitrate(tcpQueueDepth: Int) {
        val mediaCodec = codec ?: return
        val overflowDrops = videoClient.getDroppedFramesOverflowCount()
        val overflowIncreased = overflowDrops > lastOverflowDropCount
        lastOverflowDropCount = overflowDrops

        var nextBitrate = targetBitrate
        if (overflowIncreased || tcpQueueDepth >= 10) {
            nextBitrate = (targetBitrate * 75L / 100L).toInt().coerceAtLeast(MIN_BITRATE)
            stableWindowCount = 0
        } else if (tcpQueueDepth >= 6) {
            nextBitrate = (targetBitrate * 90L / 100L).toInt().coerceAtLeast(MIN_BITRATE)
            stableWindowCount = 0
        } else if (tcpQueueDepth <= 2) {
            stableWindowCount++
            if (stableWindowCount >= 3) {
                nextBitrate = (targetBitrate + 2_000_000).coerceAtMost(MAX_BITRATE)
                stableWindowCount = 0
            }
        } else {
            stableWindowCount = 0
        }

        if (nextBitrate == targetBitrate) {
            return
        }
        targetBitrate = nextBitrate
        try {
            val params = android.os.Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, targetBitrate)
            }
            mediaCodec.setParameters(params)
            if (overflowIncreased) {
                requestKeyFrame()
            }
            Log.i(TAG, "Adjusted encoder bitrate to $targetBitrate (tcp_send_q_depth=$tcpQueueDepth overflowDrops=$overflowDrops)")
        } catch (e: Exception) {
            Log.w(TAG, "Failed to adjust bitrate to $targetBitrate: ${e.message}")
        }
    }

    private fun findAvcEncoderWithSurface(): MediaCodecInfo? {
        val codecInfos = MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos
        for (codecInfo in codecInfos) {
            if (!codecInfo.isEncoder) {
                continue
            }
            val supportsAvc = codecInfo.supportedTypes.any { it.equals(MIME_TYPE, ignoreCase = true) }
            if (!supportsAvc) {
                continue
            }
            val capabilities = try {
                codecInfo.getCapabilitiesForType(MIME_TYPE)
            } catch (_: Exception) {
                continue
            }
            val supportsSurface = capabilities.colorFormats.contains(MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            if (!supportsSurface) {
                continue
            }
            return codecInfo
        }
        return null
    }

    private fun resolveEncodeSize(codecInfo: MediaCodecInfo, requestWidth: Int, requestHeight: Int): Pair<Int, Int>? {
        val capabilities = codecInfo.getCapabilitiesForType(MIME_TYPE)
        val videoCapabilities = capabilities.videoCapabilities ?: return null

        if (videoCapabilities.isSizeSupported(requestWidth, requestHeight)) {
            return Pair(requestWidth, requestHeight)
        }
        return null
    }

    private fun toAnnexB(nal: ByteArray): ByteArray {
        if (nal.size >= 4 &&
            nal[0] == 0.toByte() &&
            nal[1] == 0.toByte() &&
            ((nal[2] == 0.toByte() && nal[3] == 1.toByte()) || nal[2] == 1.toByte())
        ) {
            return nal
        }
        val startCode = byteArrayOf(0x00, 0x00, 0x00, 0x01)
        val out = ByteArray(startCode.size + nal.size)
        System.arraycopy(startCode, 0, out, 0, startCode.size)
        System.arraycopy(nal, 0, out, startCode.size, nal.size)
        return out
    }

    private fun normalizeAccessUnitToAnnexB(accessUnit: ByteArray): ByteArray? {
        if (accessUnit.isEmpty()) {
            return null
        }
        val annexB = if (isAnnexB(accessUnit)) {
            accessUnit
        } else {
            avccToAnnexB(accessUnit) ?: return null
        }
        return ensureFourByteStartCode(annexB)
    }

    private fun isAnnexB(data: ByteArray): Boolean {
        if (data.size < 4) {
            return false
        }
        if (data[0] == 0.toByte() && data[1] == 0.toByte() && data[2] == 1.toByte()) {
            return true
        }
        return data[0] == 0.toByte() &&
            data[1] == 0.toByte() &&
            data[2] == 0.toByte() &&
            data[3] == 1.toByte()
    }

    private fun avccToAnnexB(avcc: ByteArray): ByteArray? {
        var offset = 0
        val output = ByteArrayOutputStream(avcc.size + 64)
        var nalCount = 0
        while (offset + 4 <= avcc.size) {
            val nalLength = ((avcc[offset].toInt() and 0xFF) shl 24) or
                ((avcc[offset + 1].toInt() and 0xFF) shl 16) or
                ((avcc[offset + 2].toInt() and 0xFF) shl 8) or
                (avcc[offset + 3].toInt() and 0xFF)
            offset += 4
            if (nalLength <= 0 || offset + nalLength > avcc.size) {
                return null
            }
            output.write(byteArrayOf(0x00, 0x00, 0x00, 0x01))
            output.write(avcc, offset, nalLength)
            offset += nalLength
            nalCount++
        }
        if (offset != avcc.size || nalCount == 0) {
            return null
        }
        return output.toByteArray()
    }

    private fun ensureFourByteStartCode(data: ByteArray): ByteArray {
        val output = ByteArrayOutputStream(data.size + 64)
        var offset = 0
        var nalCount = 0
        while (offset < data.size) {
            var startCodeSize = 0
            if (offset + 3 < data.size &&
                data[offset] == 0.toByte() &&
                data[offset + 1] == 0.toByte() &&
                data[offset + 2] == 1.toByte()
            ) {
                startCodeSize = 3
            } else if (offset + 4 < data.size &&
                data[offset] == 0.toByte() &&
                data[offset + 1] == 0.toByte() &&
                data[offset + 2] == 0.toByte() &&
                data[offset + 3] == 1.toByte()
            ) {
                startCodeSize = 4
            }
            if (startCodeSize == 0) {
                offset++
                continue
            }

            val nalStart = offset + startCodeSize
            var nextStart = nalStart
            while (nextStart < data.size) {
                val hasThreeByte = nextStart + 2 < data.size &&
                    data[nextStart] == 0.toByte() &&
                    data[nextStart + 1] == 0.toByte() &&
                    data[nextStart + 2] == 1.toByte()
                val hasFourByte = nextStart + 3 < data.size &&
                    data[nextStart] == 0.toByte() &&
                    data[nextStart + 1] == 0.toByte() &&
                    data[nextStart + 2] == 0.toByte() &&
                    data[nextStart + 3] == 1.toByte()
                if (hasThreeByte || hasFourByte) {
                    break
                }
                nextStart++
            }
            if (nalStart >= nextStart) {
                offset = nextStart
                continue
            }
            output.write(byteArrayOf(0x00, 0x00, 0x00, 0x01))
            output.write(data, nalStart, nextStart - nalStart)
            nalCount++
            offset = nextStart
        }
        if (nalCount == 0) {
            return data
        }
        return output.toByteArray()
    }

    class Holder {
        companion object {
            val instance: StereoVideoEncoder by lazy {
                StereoVideoEncoder(WindowsVideoTcpClient.getInstance())
            }
        }
    }
}
