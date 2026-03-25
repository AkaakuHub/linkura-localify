package io.github.chocolzs.linkura.localify.ipc

import android.util.Log
import io.github.chocolzs.linkura.localify.LinkuraHookMain
import java.io.EOFException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class WindowsInputTcpClient {
    companion object {
        private const val TAG = "WindowsInputTcpClient"
        private const val HOST = "10.0.2.2"
        private const val INPUT_PAYLOAD_SIZE = 108
        private const val INPUT_TIMEOUT_MS = 300L
    }

    private val started = AtomicBoolean(false)
    @Volatile
    private var port: Int = 39200
    @Volatile
    private var socket: Socket? = null
    @Volatile
    private var loopThread: Thread? = null
    private var inputWatchdogExecutor: ScheduledExecutorService? = null
    @Volatile
    private var lastInputPacketAtMs: Long = 0L
    @Volatile
    private var zeroInputApplied = true

    fun setPort(value: Int) {
        port = if (value in 1..65535) value else 39200
    }

    fun start() {
        if (!started.compareAndSet(false, true)) {
            return
        }
        lastInputPacketAtMs = System.currentTimeMillis()
        startInputWatchdog()
        val thread = Thread({ runConnectionLoop() }, "WindowsInputTcpClient")
        thread.isDaemon = true
        loopThread = thread
        thread.start()
    }

    fun stop() {
        if (!started.compareAndSet(true, false)) {
            return
        }
        closeSocket()
        stopInputWatchdog()
        applyZeroInput()
        loopThread?.interrupt()
        loopThread = null
    }

    private fun runConnectionLoop() {
        while (started.get()) {
            try {
                val currentPort = port
                val clientSocket = Socket()
                clientSocket.tcpNoDelay = true
                clientSocket.connect(InetSocketAddress(HOST, currentPort), 2000)
                clientSocket.soTimeout = 1000
                socket = clientSocket
                Log.d(TAG, "Connected to Windows input server: $HOST:$currentPort")
                readPayloadLoop(clientSocket)
            } catch (e: Exception) {
                if (started.get()) {
                    Log.w(TAG, "Windows input connection failed: ${e.message}")
                    applyZeroInput()
                    try {
                        Thread.sleep(1000)
                    } catch (_: InterruptedException) {
                        Thread.currentThread().interrupt()
                    }
                }
            } finally {
                closeSocket()
            }
        }
    }

    private fun readPayloadLoop(clientSocket: Socket) {
        val input = clientSocket.getInputStream()
        val payload = ByteArray(INPUT_PAYLOAD_SIZE)
        while (started.get() && !clientSocket.isClosed) {
            readFully(input, payload)
            handleInputPayload(payload)
        }
    }

    private fun readFully(input: java.io.InputStream, payload: ByteArray) {
        var offset = 0
        while (offset < payload.size) {
            val read = input.read(payload, offset, payload.size - offset)
            if (read < 0) {
                throw EOFException("Input TCP stream closed")
            }
            offset += read
        }
    }

    private fun handleInputPayload(payload: ByteArray) {
        val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        LinkuraHookMain.applyWindowsCameraInput(
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.int,
            bb.int,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float,
            bb.float
        )
        zeroInputApplied = false
        lastInputPacketAtMs = System.currentTimeMillis()
    }

    private fun startInputWatchdog() {
        stopInputWatchdog()
        inputWatchdogExecutor = Executors.newSingleThreadScheduledExecutor { runnable ->
            Thread(runnable, "WindowsInputTcpWatchdog").apply { isDaemon = true }
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
        inputWatchdogExecutor?.shutdownNow()
        inputWatchdogExecutor = null
    }

    private fun closeSocket() {
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        socket = null
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
            1.0f,
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
}
