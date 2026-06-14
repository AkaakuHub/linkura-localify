package io.github.chocolzs.linkura.localify

import android.app.AndroidAppHelper
import android.content.Context
import android.net.Uri
import android.util.Base64
import android.util.Log
import android.webkit.CookieManager
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebViewClient
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.XposedHelpers
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayInputStream
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.Locale

class WebViewApiCapture {
    private val hookedClientClasses = mutableSetOf<String>()

    fun install(classLoader: ClassLoader) {
        hookClientClass(WebViewClient::class.java)

        val webViewClass = XposedHelpers.findClass("android.webkit.WebView", classLoader)
        XposedHelpers.findAndHookMethod(
            webViewClass,
            "setWebViewClient",
            WebViewClient::class.java,
            object : XC_MethodHook() {
                override fun afterHookedMethod(param: MethodHookParam) {
                    val client = param.args[0] as? WebViewClient ?: return
                    hookClientClass(client.javaClass)
                }
            }
        )
    }

    private fun hookClientClass(clientClass: Class<*>) {
        val className = clientClass.name
        synchronized(hookedClientClasses) {
            if (!hookedClientClasses.add(className)) {
                return
            }
        }

        try {
            XposedBridge.hookAllMethods(
                clientClass,
                "shouldInterceptRequest",
                object : XC_MethodHook() {
                    override fun beforeHookedMethod(param: MethodHookParam) {
                        val request = param.args.firstOrNull { it is WebResourceRequest } as? WebResourceRequest
                            ?: return
                        val response = capture(request) ?: return
                        param.result = response
                    }
                }
            )
            Log.i(TAG, "WebView API capture hooked: $className")
        } catch (e: Throwable) {
            synchronized(hookedClientClasses) {
                hookedClientClasses.remove(className)
            }
            Log.w(TAG, "Failed to hook WebViewClient.shouldInterceptRequest: $className", e)
        }
    }

    private fun capture(request: WebResourceRequest): WebResourceResponse? {
        val uri = request.url ?: return null
        if (!shouldCapture(uri, request.method)) {
            return null
        }

        val url = uri.toString()
        return try {
            fetchAndRecord(url, request.requestHeaders.orEmpty())
        } catch (e: Throwable) {
            appendCaptureError(url, request.method, e)
            null
        }
    }

    private fun shouldCapture(uri: Uri, method: String): Boolean {
        return method.equals("GET", ignoreCase = true)
            && uri.scheme == "https"
            && uri.host == "link-like-lovelive.app"
            && uri.path?.startsWith("/api/") == true
    }

    private fun fetchAndRecord(url: String, requestHeaders: Map<String, String>): WebResourceResponse? {
        val headers = requestHeaders.toMutableMap()
        val cookie = CookieManager.getInstance().getCookie(url)
        if (!cookie.isNullOrBlank() && headers.keys.none { it.equals("cookie", ignoreCase = true) }) {
            headers["Cookie"] = cookie
        }

        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 10000
            readTimeout = 10000
            instanceFollowRedirects = false
            headers.forEach { (name, value) ->
                if (!name.equals("accept-encoding", ignoreCase = true)) {
                    setRequestProperty(name, value)
                }
            }
        }

        val statusCode = connection.responseCode
        val body = (if (statusCode in 200..399) connection.inputStream else connection.errorStream)
            ?.use { it.readBytes() }
            ?: ByteArray(0)
        val responseHeaders = connection.headerFields
        appendCaptureSuccess(url, headers, statusCode, connection.responseMessage, responseHeaders, body)

        if (statusCode in 300..399) {
            return null
        }

        val contentType = responseHeaders.entries
            .firstOrNull { it.key?.equals("content-type", ignoreCase = true) == true }
            ?.value
            ?.firstOrNull()
            ?: "application/json; charset=utf-8"
        val response = WebResourceResponse(
            contentType.substringBefore(";").trim().ifBlank { "application/json" },
            extractCharset(contentType),
            ByteArrayInputStream(body)
        )
        response.setStatusCodeAndReasonPhrase(statusCode, connection.responseMessage?.ifBlank { null } ?: "OK")
        response.responseHeaders = toWebResourceResponseHeaders(responseHeaders)
        return response
    }

    private fun extractCharset(contentType: String): String {
        return contentType
            .split(";")
            .map { it.trim() }
            .firstOrNull { it.lowercase(Locale.ROOT).startsWith("charset=") }
            ?.substringAfter("=")
            ?.ifBlank { null }
            ?: "utf-8"
    }

    private fun toWebResourceResponseHeaders(headers: Map<String?, List<String>>): Map<String, String> {
        return headers
            .filterKeys { it != null }
            .filterKeys { !it.equals("content-length", ignoreCase = true) }
            .filterKeys { !it.equals("content-encoding", ignoreCase = true) }
            .mapKeys { it.key.orEmpty() }
            .mapValues { it.value.joinToString(", ") }
    }

    private fun appendCaptureSuccess(
        url: String,
        requestHeaders: Map<String, String>,
        statusCode: Int,
        statusMessage: String?,
        responseHeaders: Map<String?, List<String>>,
        body: ByteArray
    ) {
        appendCaptureEvent(
            JSONObject()
                .put("kind", "webview_api_response")
                .put("url", url)
                .put("method", "GET")
                .put("request_headers", JSONObject(requestHeaders))
                .put("status_code", statusCode)
                .put("status_message", statusMessage ?: "")
                .put("response_headers", headersToJson(responseHeaders))
                .put("body_sha256", sha256(body))
                .put("body_base64", Base64.encodeToString(body, Base64.NO_WRAP))
                .put("captured_at_ms", System.currentTimeMillis())
        )
    }

    private fun appendCaptureError(url: String, method: String, error: Throwable) {
        appendCaptureEvent(
            JSONObject()
                .put("kind", "webview_api_capture_error")
                .put("url", url)
                .put("method", method)
                .put("error", error.toString())
                .put("captured_at_ms", System.currentTimeMillis())
        )
    }

    private fun appendCaptureEvent(event: JSONObject) {
        val context = AndroidAppHelper.currentApplication()?.applicationContext ?: return
        val file = File(context.filesDir, "official_webview_api_dump.jsonl")
        synchronized(WebViewApiCapture::class.java) {
            file.appendText(event.toString() + "\n", Charsets.UTF_8)
        }
    }

    private fun headersToJson(headers: Map<String?, List<String>>): JSONArray {
        val output = JSONArray()
        headers.forEach { (name, values) ->
            if (name != null) {
                output.put(
                    JSONObject()
                        .put("name", name)
                        .put("values", JSONArray(values))
                )
            }
        }
        return output
    }

    private fun sha256(bytes: ByteArray): String {
        return MessageDigest.getInstance("SHA-256")
            .digest(bytes)
            .joinToString("") { "%02x".format(it) }
    }
}
