package io.github.chocolzs.linkura.localify.ipc

import android.util.Log
import io.github.chocolzs.linkura.localify.LinkuraHookMain
import io.github.chocolzs.linkura.localify.TAG
import io.github.chocolzs.linkura.localify.mainUtils.LogExporter
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.BufferedInputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.coroutines.coroutineContext

class WindowsInputTcpClient private constructor() {
    companion object {
        private const val CLIENT_TAG = "WindowsInputTcpClient"
        private const val HOST = "10.0.2.2"
        private const val PORT = 39090
        private const val PACKET_SIZE = 80
        private const val MAGIC = 0x4C4D4554
        private const val PROTOCOL_VERSION = 1
        private const val BODY_LENGTH = 72
        private const val CONNECT_TIMEOUT_MS = 2000
        private const val SO_TIMEOUT_MS = 250
        private const val RECONNECT_INTERVAL_MS = 1000L
        private const val INPUT_TIMEOUT_MS = 300L

        @Volatile
        private var INSTANCE: WindowsInputTcpClient? = null

        fun getInstance(): WindowsInputTcpClient {
            return INSTANCE ?: synchronized(this) {
                INSTANCE ?: WindowsInputTcpClient().also { INSTANCE = it }
            }
        }
    }

    private val isRunning = AtomicBoolean(false)
    private val isConnected = AtomicBoolean(false)
    private var clientJob: Job? = null
    private var lastPacketTimeMs: Long = 0L
    private var zeroInputApplied = true

    private data class InputPacket(
        val flags: Int,
        val leftStickX: Float,
        val leftStickY: Float,
        val rightStickX: Float,
        val rightStickY: Float,
        val leftTrigger: Float,
        val leftGrip: Float,
        val rightTrigger: Float,
        val rightGrip: Float,
        val yaw: Float,
        val pitch: Float,
        val roll: Float,
        val hmdPosX: Float,
        val hmdPosY: Float,
        val hmdPosZ: Float,
        val buttons: Int
    )

    @OptIn(DelicateCoroutinesApi::class)
    fun startClient(): Boolean {
        if (isRunning.get()) {
            Log.w(CLIENT_TAG, "TCP client already running")
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
        applyZeroInput()
    }

    private suspend fun runClientLoop() {
        while (isRunning.get() && coroutineContext.isActive) {
            try {
                Socket().use { socket ->
                    socket.tcpNoDelay = true
                    socket.soTimeout = SO_TIMEOUT_MS
                    socket.connect(InetSocketAddress(HOST, PORT), CONNECT_TIMEOUT_MS)

                    isConnected.set(true)
                    lastPacketTimeMs = System.currentTimeMillis()
                    zeroInputApplied = true
                    Log.i(CLIENT_TAG, "Connected to $HOST:$PORT")
                    LogExporter.addLogEntry(TAG, "I", "Windows input TCP connected to $HOST:$PORT")

                    readPackets(socket)
                }
            } catch (e: Exception) {
                if (isRunning.get()) {
                    Log.w(CLIENT_TAG, "TCP loop error: ${e.message}")
                    LogExporter.addLogEntry(TAG, "W", "Windows input TCP error: ${e.message}")
                }
            } finally {
                isConnected.set(false)
                applyZeroInput()
            }

            if (isRunning.get() && coroutineContext.isActive) {
                delay(RECONNECT_INTERVAL_MS)
            }
        }
    }

    private fun readPackets(socket: Socket) {
        val input = BufferedInputStream(socket.getInputStream())
        val buffer = ByteArray(PACKET_SIZE)

        while (isRunning.get() && isConnected.get()) {
            try {
                readExact(input, buffer, PACKET_SIZE)
                val packet = parsePacket(buffer) ?: continue
                LinkuraHookMain.applyWindowsCameraInput(
                    packet.leftStickX,
                    packet.leftStickY,
                    packet.rightStickX,
                    packet.rightStickY,
                    packet.leftTrigger,
                    packet.leftGrip,
                    packet.rightTrigger,
                    packet.rightGrip,
                    packet.yaw,
                    packet.pitch,
                    packet.roll,
                    packet.hmdPosX,
                    packet.hmdPosY,
                    packet.hmdPosZ,
                    packet.buttons,
                    packet.flags
                )
                lastPacketTimeMs = System.currentTimeMillis()
                zeroInputApplied = false
            } catch (_: SocketTimeoutException) {
                val now = System.currentTimeMillis()
                if (now - lastPacketTimeMs > INPUT_TIMEOUT_MS) {
                    applyZeroInput()
                }
            }
        }
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
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            0,
            0
        )
        zeroInputApplied = true
    }

    private fun parsePacket(packet: ByteArray): InputPacket? {
        if (packet.size != PACKET_SIZE) {
            return null
        }
        val bb = ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN)
        val magic = bb.int
        val version = bb.get().toInt() and 0xFF
        val flags = bb.get().toInt() and 0xFF
        val length = bb.short.toInt() and 0xFFFF
        if (magic != MAGIC || version != PROTOCOL_VERSION || length != BODY_LENGTH) {
            return null
        }

        bb.int
        bb.long
        val leftStickX = bb.float
        val leftStickY = bb.float
        val rightStickX = bb.float
        val rightStickY = bb.float
        val leftTrigger = bb.float
        val leftGrip = bb.float
        val rightTrigger = bb.float
        val rightGrip = bb.float
        val yaw = bb.float
        val pitch = bb.float
        val roll = bb.float
        val hmdPosX = bb.float
        val hmdPosY = bb.float
        val hmdPosZ = bb.float
        val buttons = bb.int

        return InputPacket(
            flags = flags,
            leftStickX = leftStickX,
            leftStickY = leftStickY,
            rightStickX = rightStickX,
            rightStickY = rightStickY,
            leftTrigger = leftTrigger,
            leftGrip = leftGrip,
            rightTrigger = rightTrigger,
            rightGrip = rightGrip,
            yaw = yaw,
            pitch = pitch,
            roll = roll,
            hmdPosX = hmdPosX,
            hmdPosY = hmdPosY,
            hmdPosZ = hmdPosZ,
            buttons = buttons
        )
    }

    private fun readExact(input: BufferedInputStream, target: ByteArray, size: Int) {
        var offset = 0
        while (offset < size) {
            val read = input.read(target, offset, size - offset)
            if (read < 0) {
                throw IllegalStateException("TCP stream closed")
            }
            offset += read
        }
    }
}
