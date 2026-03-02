package io.github.chocolzs.linkura.localify.ipc

import android.util.Log
import io.github.chocolzs.linkura.localify.mainUtils.LogExporter
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.BufferedOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.coroutineContext

class WindowsVideoTcpClient private constructor() {
    companion object {
        private const val CLIENT_TAG = "WindowsVideoTcpClient"
        private const val HOST = "10.0.2.2"
        private const val PORT = 39100

        private const val MAGIC = 0x544D564C
        private const val VERSION = 1
        private const val HEADER_SIZE = 22
        private const val MAX_PAYLOAD_LENGTH = 4 * 1024 * 1024

        private const val FLAG_KEY_FRAME = 1
        private const val FLAG_CODEC_CONFIG = 1 shl 1

        private const val CONNECT_TIMEOUT_MS = 2000
        private const val RECONNECT_INTERVAL_MS = 1000L

        @Volatile
        private var INSTANCE: WindowsVideoTcpClient? = null

        fun getInstance(): WindowsVideoTcpClient {
            return INSTANCE ?: synchronized(this) {
                INSTANCE ?: WindowsVideoTcpClient().also { INSTANCE = it }
            }
        }
    }

    private data class VideoFrame(
        val seq: Long,
        val timestampMs: Long,
        val flags: Int,
        val payload: ByteArray
    )

    private val isRunning = AtomicBoolean(false)
    private var clientJob: Job? = null

    private val latestFrame = AtomicReference<VideoFrame?>(null)
    private val sequence = AtomicLong(0L)
    private val droppedFrames = AtomicLong(0L)
    private val sentFrames = AtomicLong(0L)
    private val connectCount = AtomicLong(0L)

    @Volatile
    private var codecConfigPayload: ByteArray? = null
    @Volatile
    private var onConnected: (() -> Unit)? = null
    @Volatile
    private var pendingConfigWithKeyFrame = true

    fun setOnConnectedListener(listener: (() -> Unit)?) {
        onConnected = listener
    }

    @OptIn(DelicateCoroutinesApi::class)
    fun startClient(): Boolean {
        if (isRunning.get()) {
            Log.w(CLIENT_TAG, "Video TCP client already running")
            return true
        }
        isRunning.set(true)
        clientJob = GlobalScope.launch(Dispatchers.IO) {
            runClientLoop()
        }
        return true
    }

    fun stopClient() {
        if (!isRunning.getAndSet(false)) {
            return
        }
        try {
            clientJob?.cancel()
        } catch (_: Exception) {
        } finally {
            clientJob = null
        }
    }

    fun enqueueAccessUnit(payload: ByteArray, codecFlags: Int) {
        if (!isRunning.get()) {
            return
        }
        if (payload.isEmpty()) {
            return
        }
        if (payload.size > MAX_PAYLOAD_LENGTH) {
            Log.w(CLIENT_TAG, "Drop AU: payload too large (${payload.size} bytes)")
            return
        }

        val isKeyFrame = (codecFlags and android.media.MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
        val hasCodecConfigFlag = (codecFlags and android.media.MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0

        if (pendingConfigWithKeyFrame) {
            if (!isKeyFrame) {
                return
            }
            val config = codecConfigPayload
            if (config != null) {
                val mergedPayload = ByteArray(config.size + payload.size)
                System.arraycopy(config, 0, mergedPayload, 0, config.size)
                System.arraycopy(payload, 0, mergedPayload, config.size, payload.size)
                val mergedFrame = VideoFrame(
                    seq = sequence.incrementAndGet(),
                    timestampMs = System.currentTimeMillis(),
                    flags = FLAG_KEY_FRAME or FLAG_CODEC_CONFIG,
                    payload = mergedPayload
                )
                pendingConfigWithKeyFrame = false
                val previous = latestFrame.getAndSet(mergedFrame)
                if (previous != null) {
                    droppedFrames.incrementAndGet()
                }
                return
            }
            return
        }

        // codec_config 単体AUを送ると同期条件を壊すため送信しない
        if (hasCodecConfigFlag && !isKeyFrame) {
            return
        }

        val flags = if (isKeyFrame) FLAG_KEY_FRAME else 0
        val frame = VideoFrame(
            seq = sequence.incrementAndGet(),
            timestampMs = System.currentTimeMillis(),
            flags = flags,
            payload = payload.copyOf()
        )

        val previous = latestFrame.getAndSet(frame)
        if (previous != null) {
            droppedFrames.incrementAndGet()
        }
    }

    fun updateCodecConfig(spsPpsPayload: ByteArray) {
        if (spsPpsPayload.isEmpty()) {
            return
        }
        codecConfigPayload = spsPpsPayload.copyOf()
        pendingConfigWithKeyFrame = true
    }

    private suspend fun runClientLoop() {
        while (isRunning.get() && coroutineContext.isActive) {
            try {
                Socket().use { socket ->
                    socket.tcpNoDelay = true
                    socket.connect(InetSocketAddress(HOST, PORT), CONNECT_TIMEOUT_MS)
                    val connected = connectCount.incrementAndGet()

                    Log.i(CLIENT_TAG, "Connected to $HOST:$PORT")
                    LogExporter.addLogEntry(CLIENT_TAG, "I", "Windows video TCP connected to $HOST:$PORT")

                    BufferedOutputStream(socket.getOutputStream()).use { output ->
                        pendingConfigWithKeyFrame = true
                        onConnected?.invoke()

                        while (isRunning.get() && coroutineContext.isActive && socket.isConnected && !socket.isClosed) {
                            val frame = latestFrame.getAndSet(null)
                            if (frame == null) {
                                delay(1)
                                continue
                            }
                            sendFrame(output, frame)
                            val totalSent = sentFrames.incrementAndGet()
                            if (totalSent % 300L == 0L) {
                                Log.i(
                                    CLIENT_TAG,
                                    "Video stats: sent=$totalSent, dropped=${droppedFrames.get()}, queued=${if (latestFrame.get() == null) 0 else 1}"
                                )
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                if (isRunning.get()) {
                    Log.w(CLIENT_TAG, "Video TCP loop error: ${e.message}")
                    LogExporter.addLogEntry(CLIENT_TAG, "W", "Windows video TCP error: ${e.message}")
                }
            }

            if (isRunning.get() && coroutineContext.isActive) {
                delay(RECONNECT_INTERVAL_MS)
            }
        }
    }

    private fun sendFrame(output: BufferedOutputStream, frame: VideoFrame) {
        val header = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN).apply {
            putInt(MAGIC)
            put(VERSION.toByte())
            put(frame.flags.toByte())
            putInt((frame.seq and 0xFFFF_FFFFL).toInt())
            putLong(frame.timestampMs)
            putInt(frame.payload.size)
        }.array()

        output.write(header)
        output.write(frame.payload)
        output.flush()
        if (sentFrames.get() < 5L) {
            Log.i(CLIENT_TAG, "Sent frame: seq=${frame.seq} bytes=${frame.payload.size} flags=${frame.flags}")
        }
    }

}
