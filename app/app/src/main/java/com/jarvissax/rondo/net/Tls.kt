package com.jarvissax.rondo.net

import android.content.Context
import java.net.InetAddress
import java.net.Socket
import java.security.KeyStore
import java.security.cert.CertificateException
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509TrustManager
import com.jarvissax.rondo.R

// Android 4.2 trusts neither Let's Encrypt root and leaves TLS 1.2 switched off. The app bundles
// the two roots a Let's Encrypt chain can end in (ISRG Root X1, and ISRG Root YR for when the X1
// cross-sign goes away), keeps trusting the system's own roots for servers with other
// certificates, and forces TLS 1.2.
object Tls {
    lateinit var trustManager: X509TrustManager
        private set
    lateinit var socketFactory: SSLSocketFactory
        private set

    fun install(context: Context) {
        val certs = CertificateFactory.getInstance("X.509")
        val store = KeyStore.getInstance(KeyStore.getDefaultType()).apply { load(null, null) }
        listOf(R.raw.isrg_root_x1 to "isrg-root-x1", R.raw.isrg_root_yr to "isrg-root-yr").forEach { (res, alias) ->
            context.resources.openRawResource(res).use { store.setCertificateEntry(alias, certs.generateCertificate(it)) }
        }
        val bundled = trustManagerFor(store)
        val system = trustManagerFor(null)
        trustManager = object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = system.checkClientTrusted(chain, authType)
            override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {
                try {
                    bundled.checkServerTrusted(chain, authType)
                } catch (e: CertificateException) {
                    system.checkServerTrusted(chain, authType)
                }
            }
            override fun getAcceptedIssuers(): Array<X509Certificate> = bundled.acceptedIssuers + system.acceptedIssuers
        }
        val tls = SSLContext.getInstance("TLSv1.2").apply { init(null, arrayOf(trustManager), null) }
        socketFactory = Tls12SocketFactory(tls.socketFactory)
        // ExoPlayer's DefaultHttpDataSource goes through HttpsURLConnection
        HttpsURLConnection.setDefaultSSLSocketFactory(socketFactory)
    }

    private fun trustManagerFor(store: KeyStore?): X509TrustManager =
        TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm()).apply { init(store) }
            .trustManagers.filterIsInstance<X509TrustManager>().first()
}

class Tls12SocketFactory(private val delegate: SSLSocketFactory) : SSLSocketFactory() {
    private fun Socket.tls12(): Socket = apply { (this as? SSLSocket)?.enabledProtocols = arrayOf("TLSv1.2") }

    override fun getDefaultCipherSuites(): Array<String> = delegate.defaultCipherSuites
    override fun getSupportedCipherSuites(): Array<String> = delegate.supportedCipherSuites
    override fun createSocket(s: Socket, host: String, port: Int, autoClose: Boolean): Socket =
        delegate.createSocket(s, host, port, autoClose).tls12()
    override fun createSocket(host: String, port: Int): Socket = delegate.createSocket(host, port).tls12()
    override fun createSocket(host: String, port: Int, localHost: InetAddress, localPort: Int): Socket =
        delegate.createSocket(host, port, localHost, localPort).tls12()
    override fun createSocket(host: InetAddress, port: Int): Socket = delegate.createSocket(host, port).tls12()
    override fun createSocket(address: InetAddress, port: Int, localAddress: InetAddress, localPort: Int): Socket =
        delegate.createSocket(address, port, localAddress, localPort).tls12()
}
