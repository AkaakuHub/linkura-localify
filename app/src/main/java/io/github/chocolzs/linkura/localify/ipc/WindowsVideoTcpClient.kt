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
        private const val MAX_SEND_QUEUE_DEPTH = 18

        private const val FLAG_KEY_FRAME = 1
        private const val FLAG_CODEC_CONFIG = 1 shl 1

        private const val CONNECT_TIMEOUT_MS = 2000
        private const val RECONNECT_INTERVAL_MS = 1000L
        private const val LOG_INTERVAL_MS = 1000L
        private const val FLUSH_INTERVAL_MS = 2L

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

    private val queueLock = Any()
    private val frameQueue = ArrayDeque<VideoFrame>()
    private val sequence = AtomicLong(0L)
    private val droppedFramesOverflow = AtomicLong(0L)
    private val droppedFramesWaitingKeyFrame = AtomicLong(0L)
    private val droppedFramesCodecConfigOnly = AtomicLong(0L)
    private val droppedFramesTooLarge = AtomicLong(0L)
    private val sentFrames = AtomicLong(0L)

    @Volatile
    private var codecConfigPayload: ByteArray? = null
    @Volatile
    private var onConnected: (() -> Unit)? = null
    @Volatile
    private var onSyncFrameRequest: (() -> Unit)? = null
    @Volatile
    private var pendingConfigWithKeyFrame = true

    fun setOnConnectedListener(listener: (() -> Unit)?) {
        onConnected = listener
    }

    fun setOnSyncFrameRequestListener(listener: (() -> Unit)?) {
        onSyncFrameRequest = listener
    }

    fun getQueueDepth(): Int {
        synchronized(queueLock) {
            return frameQueue.size
        }
    }

    fun getDroppedFramesOverflowCount(): Long {
        return droppedFramesOverflow.get()
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
        synchronized(queueLock) {
            frameQueue.clear()
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
            droppedFramesTooLarge.incrementAndGet()
            return
        }

        val isKeyFrame = (codecFlags and android.media.MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
        val hasCodecConfigFlag = (codecFlags and android.media.MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0

        synchronized(queueLock) {
            if (pendingConfigWithKeyFrame) {
                if (!isKeyFrame) {
                    droppedFramesWaitingKeyFrame.incrementAndGet()
                    return
                }
                val config = codecConfigPayload
                if (config == null) {
                    droppedFramesWaitingKeyFrame.incrementAndGet()
                    return
                }
                val mergedPayload = ByteArray(config.size + payload.size)
                System.arraycopy(config, 0, mergedPayload, 0, config.size)
                System.arraycopy(payload, 0, mergedPayload, config.size, payload.size)
                val mergedFrame = VideoFrame(
                    seq = sequence.getAndIncrement(),
                    timestampMs = System.currentTimeMillis(),
                    flags = FLAG_KEY_FRAME or FLAG_CODEC_CONFIG,
                    payload = mergedPayload
                )
                pendingConfigWithKeyFrame = false
                frameQueue.clear()
                frameQueue.addLast(mergedFrame)
                return
            }

            if (hasCodecConfigFlag && !isKeyFrame) {
                droppedFramesCodecConfigOnly.incrementAndGet()
                return
            }

            if (frameQueue.size >= MAX_SEND_QUEUE_DEPTH) {
                droppedFramesOverflow.incrementAndGet()
                frameQueue.clear()
                pendingConfigWithKeyFrame = true
                requestSyncFrame("queue_overflow")
                droppedFramesWaitingKeyFrame.incrementAndGet()
                return
            }

            val flags = if (isKeyFrame) FLAG_KEY_FRAME else 0
            val frame = VideoFrame(
                seq = sequence.getAndIncrement(),
                timestampMs = System.currentTimeMillis(),
                flags = flags,
                payload = payload.copyOf()
            )
            frameQueue.addLast(frame)
        }
    }

    fun updateCodecConfig(spsPpsPayload: ByteArray) {
        if (spsPpsPayload.isEmpty()) {
            return
        }
        codecConfigPayload = spsPpsPayload.copyOf()
        synchronized(queueLock) {
            pendingConfigWithKeyFrame = true
        }
        requestSyncFrame("codec_config_updated")
    }

    private suspend fun runClientLoop() {
        while (isRunning.get() && coroutineContext.isActive) {
            try {
                Socket().use { socket ->
                    socket.tcpNoDelay = true
                    socket.connect(InetSocketAddress(HOST, PORT), CONNECT_TIMEOUT_MS)

                    Log.i(CLIENT_TAG, "Connected to $HOST:$PORT")
                    LogExporter.addLogEntry(CLIENT_TAG, "I", "Windows video TCP connected to $HOST:$PORT")

                    BufferedOutputStream(socket.getOutputStream()).use { output ->
                        synchronized(queueLock) {
                            pendingConfigWithKeyFrame = true
                            frameQueue.clear()
                        }
                        onConnected?.invoke()
                        requestSyncFrame("tcp_connected")
                        var sentFramesInWindow = 0L
                        var sentBytesInWindow = 0L
                        var lastLogTimeMs = System.currentTimeMillis()
                        var lastFlushTimeMs = lastLogTimeMs

                        while (isRunning.get() && coroutineContext.isActive && socket.isConnected && !socket.isClosed) {
                            val frame = dequeueFrame()
                            if (frame == null) {
                                maybeLogStats(lastLogTimeMs, sentFramesInWindow, sentBytesInWindow)
                                val nowMs = System.currentTimeMillis()
                                if (nowMs - lastLogTimeMs >= LOG_INTERVAL_MS) {
                                    sentFramesInWindow = 0L
                                    sentBytesInWindow = 0L
                                    lastLogTimeMs = nowMs
                                }
                                delay(1)
                                continue
                            }

                            val shouldFlush = isKeyFrame(frame.flags) || (System.currentTimeMillis() - lastFlushTimeMs >= FLUSH_INTERVAL_MS)
                            sendFrame(output, frame, shouldFlush)
                            if (shouldFlush) {
                                lastFlushTimeMs = System.currentTimeMillis()
                            }
                            sentFrames.incrementAndGet()
                            sentFramesInWindow++
                            sentBytesInWindow += frame.payload.size.toLong()
                            maybeLogStats(lastLogTimeMs, sentFramesInWindow, sentBytesInWindow)
                            val nowMs = System.currentTimeMillis()
                            if (nowMs - lastLogTimeMs >= LOG_INTERVAL_MS) {
                                sentFramesInWindow = 0L
                                sentBytesInWindow = 0L
                                lastLogTimeMs = nowMs
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

    private fun sendFrame(output: BufferedOutputStream, frame: VideoFrame, flushNow: Boolean) {
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
        if (flushNow) {
            output.flush()
        }
        if (sentFrames.get() < 5L) {
            Log.i(CLIENT_TAG, "Sent frame: seq=${frame.seq} bytes=${frame.payload.size} flags=${frame.flags}")
        }
    }

    private fun dequeueFrame(): VideoFrame? {
        synchronized(queueLock) {
            if (frameQueue.isEmpty()) {
                return null
            }
            return frameQueue.removeFirst()
        }
    }

    private fun requestSyncFrame(reason: String) {
        onSyncFrameRequest?.invoke()
        Log.i(CLIENT_TAG, "Request sync frame: reason=$reason")
    }

    private fun maybeLogStats(windowStartMs: Long, sentFramesInWindow: Long, sentBytesInWindow: Long) {
        val nowMs = System.currentTimeMillis()
        val elapsed = nowMs - windowStartMs
        if (elapsed < LOG_INTERVAL_MS) {
            return
        }
        val bitrate = if (elapsed > 0) (sentBytesInWindow * 8_000L) / elapsed else 0L
        val queueDepth = getQueueDepth()
        Log.i(
            CLIENT_TAG,
            "tcp_video_stats sent_fps=$sentFramesInWindow bitrate_bps=$bitrate tcp_send_q_depth=$queueDepth drop_overflow=${droppedFramesOverflow.get()} drop_wait_key=${droppedFramesWaitingKeyFrame.get()} drop_cfg_only=${droppedFramesCodecConfigOnly.get()} drop_too_large=${droppedFramesTooLarge.get()}"
        )
    }

    private fun isKeyFrame(flags: Int): Boolean {
        return (flags and FLAG_KEY_FRAME) != 0
    }

}
