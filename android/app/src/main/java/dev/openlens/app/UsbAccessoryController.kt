// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.app

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.FileInputStream
import java.io.FileOutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

data class UsbHostSnapshot(
    val connected: Boolean = false,
    val detail: String = "USB is not connected.",
)

object UsbHostStatus {
    @Volatile
    var snapshot = UsbHostSnapshot()
}

/**
 * Serves OpenLens over Android Open Accessory. The desktop switches the phone
 * into accessory mode and owns the bulk endpoints; this controller bridges the
 * accessory byte stream to a loopback socket so the exact same TLS-client and
 * pairing path as Wi-Fi runs on top of it.
 */
class UsbAccessoryController(context: Context, private val host: WifiHostController) : AutoCloseable {
    private val applicationContext = context.applicationContext
    private val usbManager = applicationContext.getSystemService(UsbManager::class.java)
    private val serving = AtomicBoolean(false)
    private val closed = AtomicBoolean(false)
    private var receiverRegistered = false

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(receiverContext: Context, intent: Intent) {
            when (intent.action) {
                ACTION_PERMISSION -> {
                    val accessory = androidx.core.content.IntentCompat.getParcelableExtra(
                        intent,
                        UsbManager.EXTRA_ACCESSORY,
                        UsbAccessory::class.java,
                    )
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    if (granted && accessory != null) {
                        attach(accessory)
                    } else {
                        UsbHostStatus.snapshot = UsbHostSnapshot(detail = "USB access was declined.")
                    }
                }
                UsbManager.ACTION_USB_ACCESSORY_DETACHED ->
                    UsbHostStatus.snapshot = UsbHostSnapshot(detail = "USB cable disconnected.")
            }
        }
    }

    fun start() {
        val filter = IntentFilter().apply {
            addAction(ACTION_PERMISSION)
            addAction(UsbManager.ACTION_USB_ACCESSORY_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            applicationContext.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            applicationContext.registerReceiver(receiver, filter)
        }
        receiverRegistered = true
        usbManager.accessoryList?.firstOrNull { it.isOpenLensDesktop() }?.let(::attach)
    }

    /** Entry point for both start-up discovery and the accessory-attached intent. */
    fun attach(accessory: UsbAccessory) {
        if (closed.get() || !accessory.isOpenLensDesktop()) return
        if (!usbManager.hasPermission(accessory)) {
            val intent = PendingIntent.getBroadcast(
                applicationContext,
                2,
                Intent(ACTION_PERMISSION).setPackage(applicationContext.packageName),
                PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            usbManager.requestPermission(accessory, intent)
            return
        }
        if (!serving.compareAndSet(false, true)) return
        thread(name = "OpenLensUsbSession") {
            try {
                serve(accessory)
            } catch (error: Throwable) {
                Log.e(TAG, "USB session failed", error)
                UsbHostStatus.snapshot =
                    UsbHostSnapshot(detail = error.message ?: "The USB connection failed.")
            } finally {
                serving.set(false)
            }
            // The desktop opens a fresh connection per request (pair, then stream),
            // so keep serving while the accessory stays attached.
            if (!closed.get()) {
                Thread.sleep(1_000)
                usbManager.accessoryList?.firstOrNull { it.isOpenLensDesktop() }?.let(::attach)
            }
        }
    }

    private fun serve(accessory: UsbAccessory) {
        val descriptor: ParcelFileDescriptor = usbManager.openAccessory(accessory)
            ?: error("Could not open the USB connection to the computer.")
        // The accessory file descriptor must stay open across desktop
        // connections: closing it ends the accessory session until the bus
        // re-enumerates. One open descriptor serves sync-separated sessions.
        descriptor.use { pfd ->
            val accessoryInput = FileInputStream(pfd.fileDescriptor)
            val accessoryOutput = FileOutputStream(pfd.fileDescriptor)
            while (!closed.get()) {
                UsbHostStatus.snapshot =
                    UsbHostSnapshot(connected = true, detail = "Waiting for the computer over USB…")
                if (!awaitDesktopSync(accessoryInput, accessoryOutput)) break
                UsbHostStatus.snapshot = UsbHostSnapshot(connected = true, detail = "Connected over USB.")
                runOneSession(accessoryInput, accessoryOutput)
                UsbHostStatus.snapshot = UsbHostSnapshot(connected = true, detail = "USB session ended.")
            }
        }
        UsbHostStatus.snapshot = UsbHostSnapshot(detail = "USB cable disconnected.")
    }

    private fun runOneSession(accessoryInput: FileInputStream, accessoryOutput: FileOutputStream) {
        // Bridge the accessory byte stream to a loopback socket pair so the
        // TLS stack sees an ordinary Socket.
        val listener = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
        val tlsSide = Socket()
        val bridgeSide: Socket
        listener.use {
            tlsSide.connect(InetSocketAddress(InetAddress.getLoopbackAddress(), it.localPort), 2_000)
            bridgeSide = it.accept()
        }
        val pumps = listOf(
            thread(name = "OpenLensUsbIn") {
                // Exits when a write to the closed bridge fails, which happens on
                // the first desktop bytes after the session ends; the desktop
                // re-sends its sync token, so nothing meaningful is lost.
                pump(accessoryInput::read, bridgeSide.getOutputStream()::write) {
                    runCatching { bridgeSide.close() }
                }
            },
            thread(name = "OpenLensUsbOut") {
                pump(bridgeSide.getInputStream()::read, accessoryOutput::write) {
                    runCatching { bridgeSide.close() }
                }
            },
        )
        try {
            host.serveSocket(tlsSide, handshakeTimeoutMillis = 15_000)
        } catch (error: Throwable) {
            Log.e(TAG, "USB session failed", error)
        } finally {
            // The camera session may still own the TLS socket; the pumps keep
            // carrying its bytes until either side closes.
            pumps.forEach { it.join() }
            runCatching { bridgeSide.close() }
            runCatching { tlsSide.close() }
        }
    }

    /**
     * Resynchronises the accessory byte stream with the desktop before TLS.
     * The desktop repeats SYN ("OLNS" + nonce); the phone echoes it back
     * ("OLNE" + nonce) and waits for GO ("OLGO" + nonce). Scanning for the
     * tokens discards stale bytes from any earlier connection, and blocking
     * here keeps the phone quiet until a desktop actually connects.
     */
    private fun awaitDesktopSync(input: FileInputStream, output: FileOutputStream): Boolean {
        val tail = ByteArray(12)
        var expectedGo: ByteArray? = null
        val chunk = ByteArray(4096)
        while (true) {
            val count = try {
                input.read(chunk)
            } catch (_: Throwable) {
                return false
            }
            if (count < 0) return false
            for (index in 0 until count) {
                System.arraycopy(tail, 1, tail, 0, tail.size - 1)
                tail[tail.size - 1] = chunk[index]
                val go = expectedGo
                if (go != null && tail.contentEquals(go)) return true
                if (tail[0] == 'O'.code.toByte() && tail[1] == 'L'.code.toByte() &&
                    tail[2] == 'N'.code.toByte() && tail[3] == 'S'.code.toByte()
                ) {
                    val nonce = tail.copyOfRange(4, 12)
                    output.write(byteArrayOf('O'.code.toByte(), 'L'.code.toByte(), 'N'.code.toByte(), 'E'.code.toByte()) + nonce)
                    output.flush()
                    expectedGo = byteArrayOf('O'.code.toByte(), 'L'.code.toByte(), 'G'.code.toByte(), 'O'.code.toByte()) + nonce
                }
            }
        }
    }

    private inline fun pump(
        read: (ByteArray, Int, Int) -> Int,
        write: (ByteArray, Int, Int) -> Unit,
        onDone: () -> Unit,
    ) {
        val buffer = ByteArray(16 * 1024)
        try {
            while (true) {
                val count = read(buffer, 0, buffer.size)
                if (count < 0) break
                if (count > 0) write(buffer, 0, count)
            }
        } catch (_: Throwable) {
        } finally {
            onDone()
        }
    }

    override fun close() {
        closed.set(true)
        if (receiverRegistered) {
            runCatching { applicationContext.unregisterReceiver(receiver) }
            receiverRegistered = false
        }
    }

    private companion object {
        const val TAG = "OpenLensUsb"
        const val ACTION_PERMISSION = "dev.openlens.app.USB_PERMISSION"
    }
}

private fun UsbAccessory.isOpenLensDesktop(): Boolean =
    manufacturer == "OpenLens" && model == "OpenLens desktop"
