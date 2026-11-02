package com.jarvissax.rondo.net

import com.google.android.exoplayer2.C
import com.google.android.exoplayer2.upstream.DefaultLoadErrorHandlingPolicy
import com.google.android.exoplayer2.upstream.HttpDataSource
import com.google.android.exoplayer2.upstream.LoadErrorHandlingPolicy
import java.io.IOException
import java.net.SocketException
import okhttp3.Interceptor
import okhttp3.Response

// The hotspot this Y1 uses rejects ~70% of new TCP connections to the internet (10 connects
// each from the device shell, 2026-10-30: 1.1.1.1 2/10, 8.8.8.8 4/10, the VPS 3/10, the LAN
// router 10/10). A rejected connect fails instantly with EHOSTUNREACH, so it is retried at once
// instead of with ExoPlayer's 1-2-3-5 s backoff, which had turned startup into 12 s.
private const val FAST_RETRY_MS = 250L
private const val CONNECT_RETRIES = 20

fun Throwable.isConnectFailure(): Boolean = generateSequence(this) { it.cause }.any { it is SocketException }

class FastRetryLoadErrorPolicy : DefaultLoadErrorHandlingPolicy(CONNECT_RETRIES) {
    override fun getRetryDelayMsFor(info: LoadErrorHandlingPolicy.LoadErrorInfo): Long = when {
        info.exception.isConnectFailure() -> FAST_RETRY_MS
        // the server can't stream this video (404 no_dash, 400 bad id): asking again won't change that
        (info.exception as? HttpDataSource.InvalidResponseCodeException)?.responseCode.let { it == 404 || it == 400 } -> C.TIME_UNSET
        else -> super.getRetryDelayMsFor(info)
    }
}

class ConnectRetryInterceptor : Interceptor {
    override fun intercept(chain: Interceptor.Chain): Response {
        var attempt = 0
        while (true) {
            try {
                return chain.proceed(chain.request())
            } catch (e: IOException) {
                if (!e.isConnectFailure() || ++attempt >= CONNECT_RETRIES) throw e
                Thread.sleep(FAST_RETRY_MS)
            }
        }
    }
}
