// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.app

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log
import androidx.core.content.ContextCompat
import dev.openlens.protocol.PairingCrypto
import java.io.InputStream
import java.net.ServerSocket
import java.net.SocketException
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.X509TrustManager
import kotlin.concurrent.thread

data class WifiHostSnapshot(
    val running: Boolean = false,
    val port: Int = 0,
    val registeredName: String = "",
    val detail: String = "Wi-Fi host is stopped.",
    val paired: Boolean = false,
)

data class WifiPairingSnapshot(
    val active: Boolean = false,
    val code: String = "",
    val detail: String = "",
)

object WifiHostStatus {
    @Volatile
    var snapshot = WifiHostSnapshot()
}

object WifiPairingStatus {
    private val decision = Object()
    @Volatile
    var snapshot = WifiPairingSnapshot()
        private set
    private var confirmed: Boolean? = null

    fun showCode(code: String) {
        synchronized(decision) {
            confirmed = null
            snapshot = WifiPairingSnapshot(
                active = true,
                code = code,
                detail = "Check that this code matches the one on your computer.",
            )
        }
    }

    fun decide(matches: Boolean) {
        synchronized(decision) {
            confirmed = matches
            decision.notifyAll()
        }
    }

    fun waitForDecision(timeoutMillis: Long): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMillis
        synchronized(decision) {
            while (confirmed == null) {
                val remaining = deadline - System.currentTimeMillis()
                if (remaining <= 0) break
                decision.wait(remaining)
            }
            return confirmed == true
        }
    }

    fun clear(detail: String = "") {
        synchronized(decision) {
            snapshot = WifiPairingSnapshot(detail = detail)
            confirmed = null
        }
    }
}

internal object WifiSocketRegistry {
    private val pending = AtomicReference<SSLSocket?>()

    fun offer(socket: SSLSocket): Boolean = pending.compareAndSet(null, socket)
    fun take(): SSLSocket? = pending.getAndSet(null)
    fun clear() = pending.getAndSet(null)?.let { runCatching { it.close() } }
}

private class WifiPeerStore(private val context: Context) {
    private val preferences = context.getSharedPreferences("openlens_wifi", Context.MODE_PRIVATE)

    fun desktopPin(): ByteArray? = preferences.getString("desktop_spki", null)?.hexBytes()
    fun saveDesktopPin(pin: ByteArray) {
        check(preferences.edit().putString("desktop_spki", pin.hex()).commit()) {
            "Could not save the paired computer."
        }
    }
    fun forget() {
        preferences.edit().remove("desktop_spki").apply()
    }
}

class WifiHostController(context: Context) : AutoCloseable {
    private val applicationContext = context.applicationContext
    private val nsd = applicationContext.getSystemService(NsdManager::class.java)
    private val peerStore = WifiPeerStore(applicationContext)
    private val running = AtomicBoolean(false)
    private val pairingInProgress = AtomicBoolean(false)
    private val activeClients = AtomicInteger(0)
    private val connectionTimes = ArrayDeque<Long>()
    private val pairingTimes = ArrayDeque<Long>()
    private var pairingWindowDeadline = System.currentTimeMillis() + 120_000
    private var server: ServerSocket? = null
    private var registration: NsdManager.RegistrationListener? = null
    private val installationId: String by lazy {
        val preferences = applicationContext.getSharedPreferences("openlens_wifi", Context.MODE_PRIVATE)
        preferences.getString("installation_id", null) ?: UUID.randomUUID().toString().also {
            preferences.edit().putString("installation_id", it).apply()
        }
    }

    private val tlsContext: SSLContext by lazy {
        val trust = object : X509TrustManager {
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                require(!chain.isNullOrEmpty()) { "Desktop did not present an identity certificate." }
            }
            override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                require(!chain.isNullOrEmpty()) { "Desktop did not present an identity certificate." }
            }
        }
        SSLContext.getInstance("TLS", org.conscrypt.Conscrypt.newProvider()).apply {
            init(WifiIdentity.keyManagers(), arrayOf(trust), SecureRandom())
        }
    }

    /**
     * Wraps an already-connected transport socket (Wi-Fi accept or the USB accessory
     * bridge) with this phone acting as the TLS client, then serves the OpenLens
     * request on it. Blocks until the request is handled; when the camera stream
     * takes over the socket, it returns while the stream continues elsewhere.
     */
    fun serveSocket(tcpSocket: java.net.Socket, handshakeTimeoutMillis: Int = 45_000) {
        runCatching { tcpSocket.tcpNoDelay = true }
        val accepted = try {
            tlsContext.socketFactory.createSocket(
                tcpSocket,
                tcpSocket.inetAddress.hostAddress,
                tcpSocket.port,
                true,
            ) as SSLSocket
        } catch (error: Throwable) {
            runCatching { tcpSocket.close() }
            throw error
        }
        accepted.useClientMode = true
        accepted.enabledProtocols = arrayOf("TLSv1.3")
        accepted.soTimeout = handshakeTimeoutMillis
        accepted.startHandshake()
        accepted.soTimeout = 45_000
        handleClient(accepted)
    }

    fun start() {
        if (!running.compareAndSet(false, true)) return
        try {
            val listener = ServerSocket(0)
            listener.reuseAddress = true
            server = listener
            register(listener.localPort)
            thread(name = "OpenLensWifiAccept") { acceptLoop(listener) }
        } catch (error: Throwable) {
            Log.e(TAG, "Could not start Wi-Fi host", error)
            running.set(false)
            WifiHostStatus.snapshot = WifiHostSnapshot(detail = error.message ?: "Could not start Wi-Fi discovery.")
            runCatching { server?.close() }
            server = null
        }
    }

    fun forgetComputer() {
        peerStore.forget()
        refreshPairingWindow()
        WifiPairingStatus.clear("Computer forgotten. Pair again from the desktop app.")
        updateAvailability("Ready to pair with a computer.")
    }

    fun refreshPairingWindow() {
        pairingWindowDeadline = System.currentTimeMillis() + 120_000
        updateAvailability("Ready to pair for the next two minutes.")
    }

    private fun register(port: Int) {
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(serviceInfo: NsdServiceInfo) {
                WifiHostStatus.snapshot = WifiHostSnapshot(
                    running = true,
                    port = port,
                    registeredName = serviceInfo.serviceName,
                    detail = if (peerStore.desktopPin() == null) {
                        "Found on Wi-Fi. Pair from the desktop app within two minutes."
                    } else {
                        "Paired and ready. Start the camera from your computer."
                    },
                    paired = peerStore.desktopPin() != null,
                )
            }

            override fun onRegistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                WifiHostStatus.snapshot = WifiHostSnapshot(detail = "Wi-Fi discovery registration failed ($errorCode).")
            }

            override fun onServiceUnregistered(serviceInfo: NsdServiceInfo) = Unit
            override fun onUnregistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = Unit
        }
        registration = listener
        val info = NsdServiceInfo().apply {
            serviceName = "OpenLens phone"
            serviceType = "_openlens._tcp."
            setPort(port)
            setAttribute("v", "2")
            setAttribute("tls", "1")
            setAttribute("pair", if (peerStore.desktopPin() == null) "1" else "0")
            setAttribute("busy", "0")
            setAttribute("id", installationId)
        }
        nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
        WifiHostStatus.snapshot = WifiHostSnapshot(
            running = true,
            port = port,
            detail = "Making OpenLens available on your local Wi-Fi…",
            paired = peerStore.desktopPin() != null,
        )
    }

    private fun acceptLoop(listener: ServerSocket) {
        while (running.get()) {
            try {
                val tcpSocket = listener.accept()
                if (!allowConnection()) {
                    runCatching { tcpSocket.close() }
                    continue
                }
                if (activeClients.incrementAndGet() > MAX_CLIENTS) {
                    activeClients.decrementAndGet()
                    runCatching { tcpSocket.close() }
                    continue
                }
                thread(name = "OpenLensWifiClient") {
                    try {
                        serveSocket(tcpSocket)
                    } catch (error: Throwable) {
                        Log.e(TAG, "Wi-Fi client failed", error)
                        if (running.get() && !pairingInProgress.get()) {
                            updateAvailability(error.message ?: "A secure Wi-Fi connection failed.")
                        }
                    } finally {
                        activeClients.decrementAndGet()
                    }
                }
            } catch (_: SocketException) {
                if (running.get()) updateAvailability("Wi-Fi listener stopped unexpectedly.")
                break
            } catch (error: Throwable) {
                Log.e(TAG, "Wi-Fi accept failed", error)
                if (running.get()) updateAvailability(error.message ?: "A Wi-Fi connection failed.")
            }
        }
    }

    private fun handleClient(socket: SSLSocket) {
        var transferred = false
        try {
            val certificate = socket.session.peerCertificates.firstOrNull() as? X509Certificate
                ?: error("Desktop did not present an identity certificate.")
            val desktopPin = MessageDigest.getInstance("SHA-256").digest(certificate.publicKey.encoded)
            when (readBoundedLine(socket.inputStream)) {
                "PAIR 2" -> handlePairing(socket, desktopPin)
                "OPENLENS 2" -> {
                    val trusted = peerStore.desktopPin()
                    require(trusted != null && PairingCrypto.constantTimeEquals(trusted, desktopPin)) {
                        "This computer is not paired with OpenLens."
                    }
                    require(applicationContext.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                        "Allow camera access in the OpenLens phone app first."
                    }
                    require(WifiSocketRegistry.offer(socket)) { "The camera is already connected." }
                    try {
                        ContextCompat.startForegroundService(
                            applicationContext,
                            Intent(applicationContext, CameraStreamingService::class.java)
                                .setAction(CameraStreamingService.ACTION_WIFI),
                        )
                        transferred = true
                    } catch (error: Throwable) {
                        WifiSocketRegistry.clear()
                        throw error
                    }
                }
                else -> error("Unknown OpenLens Wi-Fi request.")
            }
        } catch (error: Throwable) {
            Log.e(TAG, "Wi-Fi client failed", error)
            if (running.get() && !pairingInProgress.get()) {
                updateAvailability(error.message ?: "A secure Wi-Fi connection failed.")
            }
        } finally {
            if (!transferred) runCatching { socket.close() }
        }
    }

    private fun handlePairing(socket: SSLSocket, desktopPin: ByteArray) {
        require(peerStore.desktopPin() == null) { "Forget the current computer on the phone before pairing another one." }
        require(System.currentTimeMillis() <= pairingWindowDeadline) { "Pairing timed out. Reopen the phone app and try again." }
        require(allowPairingAttempt()) { "Too many pairing attempts. Reopen the phone app before retrying." }
        require(pairingInProgress.compareAndSet(false, true)) { "Another pairing attempt is already active." }
        try {
            val random = SecureRandom()
            val attempt = ByteArray(32).also(random::nextBytes)
            val phoneNonce = ByteArray(32).also(random::nextBytes)
            val phonePin = WifiIdentity.spkiPin()
            val phoneCommitment = PairingCrypto.commitment(
                attempt,
                PairingCrypto.Role.PHONE,
                phoneNonce,
                phonePin,
                desktopPin,
            )
            socket.writeLine("PAIR1 ${attempt.hex()} ${phoneCommitment.hex()} ${phonePin.hex()}")
            val commit = readBoundedLine(socket.inputStream).split(' ')
            require(commit.size == 3 && commit[0] == "PAIR2") { "Invalid desktop pairing commitment." }
            val desktopCommitment = commit[1].hexBytes()
            val claimedDesktopPin = commit[2].hexBytes()
            require(PairingCrypto.constantTimeEquals(claimedDesktopPin, desktopPin)) {
                "Desktop pairing identity did not match its TLS certificate."
            }
            socket.writeLine("PAIR3 ${phoneNonce.hex()}")
            val reveal = readBoundedLine(socket.inputStream).split(' ')
            require(reveal.size == 2 && reveal[0] == "PAIR4") { "Invalid desktop pairing reveal." }
            val desktopNonce = reveal[1].hexBytes()
            val expected = PairingCrypto.commitment(
                attempt,
                PairingCrypto.Role.DESKTOP,
                desktopNonce,
                phonePin,
                desktopPin,
            )
            require(PairingCrypto.constantTimeEquals(expected, desktopCommitment)) {
                "Desktop pairing commitment verification failed."
            }
            val sas = PairingCrypto.sixDigitSas(
                PairingCrypto.transcript(
                    attempt,
                    phonePin,
                    desktopPin,
                    phoneCommitment,
                    desktopCommitment,
                    phoneNonce,
                    desktopNonce,
                ),
            )
            WifiPairingStatus.showCode(sas)
            val desktopConfirmation = AtomicBoolean(false)
            val desktopDecision = Object()
            val confirmationReader = thread(name = "OpenLensPairConfirm") {
                desktopConfirmation.set(runCatching { readBoundedLine(socket.inputStream) == "PAIR_CONFIRM" }.getOrDefault(false))
                synchronized(desktopDecision) { desktopDecision.notifyAll() }
            }
            val localConfirmed = WifiPairingStatus.waitForDecision(30_000)
            synchronized(desktopDecision) {
                if (!desktopConfirmation.get()) desktopDecision.wait(30_000)
            }
            confirmationReader.join(250)
            require(localConfirmed && desktopConfirmation.get()) { "Pairing was cancelled or timed out." }
            peerStore.saveDesktopPin(desktopPin)
            socket.writeLine("PAIR_OK")
            WifiPairingStatus.clear("Paired successfully.")
            updateAvailability("Paired and ready. Start the camera from your computer.")
        } finally {
            pairingInProgress.set(false)
            if (WifiPairingStatus.snapshot.active) WifiPairingStatus.clear("Pairing ended.")
        }
    }

    private fun updateAvailability(detail: String) {
        WifiHostStatus.snapshot = WifiHostStatus.snapshot.copy(
            running = running.get(),
            detail = detail,
            paired = peerStore.desktopPin() != null,
        )
    }

    private fun allowConnection(): Boolean = synchronized(connectionTimes) {
        val cutoff = System.currentTimeMillis() - 60_000
        while (connectionTimes.firstOrNull()?.let { it < cutoff } == true) connectionTimes.removeFirst()
        if (connectionTimes.size >= MAX_CONNECTIONS_PER_MINUTE) {
            false
        } else {
            connectionTimes.addLast(System.currentTimeMillis())
            true
        }
    }

    private fun allowPairingAttempt(): Boolean = synchronized(pairingTimes) {
        val cutoff = System.currentTimeMillis() - 120_000
        while (pairingTimes.firstOrNull()?.let { it < cutoff } == true) pairingTimes.removeFirst()
        if (pairingTimes.size >= MAX_PAIRING_ATTEMPTS) {
            false
        } else {
            pairingTimes.addLast(System.currentTimeMillis())
            true
        }
    }

    override fun close() {
        if (!running.getAndSet(false)) return
        registration?.let { runCatching { nsd.unregisterService(it) } }
        registration = null
        runCatching { server?.close() }
        server = null
        WifiSocketRegistry.clear()
        WifiPairingStatus.decide(false)
        WifiPairingStatus.clear()
        WifiHostStatus.snapshot = WifiHostSnapshot()
    }

    private companion object {
        const val TAG = "OpenLensWifi"
        const val MAX_CLIENTS = 3
        const val MAX_CONNECTIONS_PER_MINUTE = 20
        const val MAX_PAIRING_ATTEMPTS = 5
    }
}

private fun readBoundedLine(input: InputStream, limit: Int = 4096): String {
    val bytes = ArrayList<Byte>()
    while (bytes.size < limit) {
        val value = input.read()
        require(value >= 0) { "Secure connection closed unexpectedly." }
        if (value == '\n'.code) return bytes.toByteArray().decodeToString().trimEnd('\r')
        bytes.add(value.toByte())
    }
    error("OpenLens Wi-Fi request was too large.")
}

private fun SSLSocket.writeLine(value: String) {
    outputStream.write("$value\n".encodeToByteArray())
    outputStream.flush()
}

private fun ByteArray.hex(): String = joinToString("") { "%02x".format(it.toUByte().toInt()) }

private fun String.hexBytes(): ByteArray {
    require(length == 64) { "Identity value must contain 64 hexadecimal characters." }
    return ByteArray(32) { index ->
        substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
}
