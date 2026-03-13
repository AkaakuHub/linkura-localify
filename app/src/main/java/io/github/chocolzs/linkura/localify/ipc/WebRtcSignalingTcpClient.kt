package io.github.chocolzs.linkura.localify.ipc

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

class WebRtcSignalingTcpClient {
    companion object {
        private const val TAG = "WebRtcSignalingTcpClient"
        private const val HOST = "10.0.2.2"
        private const val DEFAULT_PORT = 39200
        private const val CONNECT_TIMEOUT_MS = 2000
        private const val RECONNECT_DELAY_MS = 1000L
    }

    private val json = Json {
        encodeDefaults = true
        ignoreUnknownKeys = true
    }
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val isRunning = AtomicBoolean(false)
    private var connectJob: Job? = null
    @Volatile
    private var port: Int = DEFAULT_PORT
    @Volatile
    private var activeSocket: Socket? = null

    @Volatile
    private var writer: BufferedWriter? = null

    @Volatile
    private var onMessage: ((WebRtcSignalingMessage) -> Unit)? = null

    @Volatile
    private var onConnected: (() -> Unit)? = null

    fun setOnMessageListener(listener: ((WebRtcSignalingMessage) -> Unit)?) {
        onMessage = listener
    }

    fun setOnConnectedListener(listener: (() -> Unit)?) {
        onConnected = listener
    }

    fun setPort(newPort: Int) {
        port = if (newPort in 1..65535) {
            newPort
        } else {
            DEFAULT_PORT
        }
    }

    fun start() {
        if (!isRunning.compareAndSet(false, true)) {
            return
        }
        connectJob = scope.launch {
            while (isActive && isRunning.get()) {
                runConnectionLoop()
                delay(RECONNECT_DELAY_MS)
            }
        }
    }

    fun stop() {
        if (!isRunning.compareAndSet(true, false)) {
            return
        }
        writer = null
        try {
            activeSocket?.close()
        } catch (_: Exception) {
        }
        activeSocket = null
        connectJob?.cancel()
        connectJob = null
    }

    fun restart() {
        val wasRunning = isRunning.get()
        stop()
        if (wasRunning) {
            start()
        }
    }

    fun send(message: WebRtcSignalingMessage): Boolean {
        val activeWriter = writer ?: return false
        return try {
            val line = json.encodeToString(message)
            synchronized(activeWriter) {
                activeWriter.write(line)
                activeWriter.write("\n")
                activeWriter.flush()
            }
            true
        } catch (e: Exception) {
            Log.w(TAG, "Failed to send signaling message: ${e.message}")
            false
        }
    }

    private suspend fun runConnectionLoop() {
        val currentPort = port
        try {
            Socket().use { socket ->
                activeSocket = socket
                socket.tcpNoDelay = true
                socket.connect(InetSocketAddress(HOST, currentPort), CONNECT_TIMEOUT_MS)

                BufferedWriter(OutputStreamWriter(socket.getOutputStream())).use { socketWriter ->
                    BufferedReader(InputStreamReader(socket.getInputStream())).use { reader ->
                        writer = socketWriter
                        Log.d(TAG, "Connected to signaling server: $HOST:$currentPort")
                        try {
                            onConnected?.invoke()
                        } catch (callbackError: Exception) {
                            Log.e(TAG, "Connected callback failed", callbackError)
                        }
                        while (isRunning.get() && reader.readLine().also { line ->
                                if (line != null) {
                                    consumeLine(line)
                                }
                            } != null
                        ) {
                            // handled in also block
                        }
                    }
                }
            }
        } catch (e: Exception) {
            if (isRunning.get()) {
                Log.w(TAG, "Signaling connection error: ${e.message}")
            }
        } finally {
            writer = null
            activeSocket = null
        }
    }

    private fun consumeLine(line: String) {
        try {
            val normalized = line.trimStart('\uFEFF').trim()
            if (normalized.isEmpty()) {
                return
            }
            val message = json.decodeFromString(WebRtcSignalingMessage.serializer(), normalized)
            try {
                onMessage?.invoke(message)
            } catch (callbackError: Exception) {
                Log.e(TAG, "Signaling callback failed", callbackError)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to parse signaling message: ${e.message}")
        }
    }
}
