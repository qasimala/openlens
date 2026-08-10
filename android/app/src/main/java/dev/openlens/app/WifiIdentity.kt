// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.app

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.MessageDigest
import java.security.Principal
import java.security.PrivateKey
import java.security.spec.ECGenParameterSpec
import java.util.Calendar
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLEngine
import javax.net.ssl.X509ExtendedKeyManager
import javax.net.ssl.X509KeyManager
import javax.security.auth.x500.X500Principal
import java.net.Socket

object WifiIdentity {
    private const val KEY_ALIAS = "openlens_wifi_identity_v1"

    fun keyManagers(): Array<javax.net.ssl.KeyManager> {
        val keyStore = loadStore()
        val factory = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm())
        factory.init(keyStore, null)
        val delegate = factory.keyManagers.filterIsInstance<X509KeyManager>().firstOrNull()
            ?: error("Android did not provide an identity key manager.")
        return arrayOf(OpenLensKeyManager(delegate))
    }

    fun spkiPin(): ByteArray {
        val certificate = requireNotNull(loadStore().getCertificate(KEY_ALIAS))
        return MessageDigest.getInstance("SHA-256").digest(certificate.publicKey.encoded)
    }

    private fun loadStore(): KeyStore {
        val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (!keyStore.containsAlias(KEY_ALIAS)) generateIdentity()
        return KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
    }

    private fun generateIdentity() {
        val now = Calendar.getInstance()
        val end = Calendar.getInstance().apply { add(Calendar.YEAR, 25) }
        val generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore")
        generator.initialize(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY,
            )
                .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
                .setDigests(KeyProperties.DIGEST_SHA256)
                .setCertificateSubject(X500Principal("CN=OpenLens phone"))
                .setCertificateSerialNumber(BigInteger(160, java.security.SecureRandom()).abs())
                .setCertificateNotBefore(now.time)
                .setCertificateNotAfter(end.time)
                .setUserAuthenticationRequired(false)
                .build(),
        )
        generator.generateKeyPair()
    }

    /** OpenLens has one TLS identity, so do not rely on vendor-specific alias selection. */
    private class OpenLensKeyManager(private val delegate: X509KeyManager) : X509ExtendedKeyManager() {
        override fun getClientAliases(keyType: String?, issuers: Array<out Principal>?): Array<String>? =
            delegate.getClientAliases(keyType, issuers)

        override fun chooseClientAlias(
            keyType: Array<out String>?,
            issuers: Array<out Principal>?,
            socket: Socket?,
        ): String? = delegate.chooseClientAlias(keyType, issuers, socket)

        override fun getServerAliases(keyType: String?, issuers: Array<out Principal>?): Array<String>? =
            arrayOf(KEY_ALIAS)

        override fun chooseServerAlias(
            keyType: String?,
            issuers: Array<out Principal>?,
            socket: Socket?,
        ): String = KEY_ALIAS

        override fun chooseEngineServerAlias(
            keyType: String?,
            issuers: Array<out Principal>?,
            engine: SSLEngine?,
        ): String = KEY_ALIAS

        override fun getCertificateChain(alias: String?): Array<java.security.cert.X509Certificate>? =
            delegate.getCertificateChain(alias)

        override fun getPrivateKey(alias: String?): PrivateKey? = delegate.getPrivateKey(alias)
    }
}
