package io.github.chocolzs.linkura.localify

import android.content.Context
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

data class OfficialUserDataDumpResult(
    val zipFile: File,
    val successCount: Int,
    val failureCount: Int,
)

object OfficialUserDataDumpExporter {
    private const val apiBaseUrl = "https://api.link-like-lovelive.app/v1"
    private const val apiKey = "4e769efa67d8f54be0b67e8f70ccb23d513a3c841191b6b2ba45ffc6fb498068"

    fun collectAndExport(context: Context): OfficialUserDataDumpResult {
        val filesDir = context.filesDir
        val existingApiDump = File(filesDir, "official_api_dump.jsonl")
        val existingAudit = File(filesDir, "official_request_audit.jsonl")
        val authContext = readLatestAuthContext(existingApiDump)

        val directApiEvents = mutableListOf<String>()
        val directAuditEvents = mutableListOf<String>()
        val coverageEntries = mutableListOf<JSONObject>()

        val plan = createCollectionPlan(authContext.playerId)
        for (target in plan) {
            val requestBody = target.requestBody.toString()
            val startedAtMs = System.currentTimeMillis()
            directAuditEvents += JSONObject()
                .put("kind", "official_user_dump_direct_request")
                .put("target", "/v1${target.path}")
                .put("detail", JSONObject().put("request", requestBody))
                .put("current_client_version", authContext.clientVersion)
                .put("current_res_version", authContext.resVersion)
                .put("captured_at_ms", startedAtMs)
                .toString()

            val result = postOfficialApi(authContext, target.path, requestBody)
            directApiEvents += result.event.toString()
            coverageEntries += JSONObject()
                .put("category", target.category)
                .put("source_endpoint", "/v1${target.path}")
                .put("request", target.requestBody)
                .put("status", if (result.isSuccess) "dumped" else "missing")
                .put("status_code", result.statusCode)
                .put("reason", result.reason)
                .put("captured_at_ms", result.capturedAtMs)
        }

        val outputDir = File(filesDir, "official_user_data_dumps").apply { mkdirs() }
        val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val zipFile = File(outputDir, "official_user_data_dump_$timestamp.zip")

        val apiDumpText = buildString {
            appendRedactedJsonlFileIfExists(existingApiDump)
            directApiEvents.forEach { append(it).append('\n') }
        }
        val auditText = buildString {
            appendRedactedJsonlFileIfExists(existingAudit)
            directAuditEvents.forEach { append(it).append('\n') }
        }
        val successCount = coverageEntries.count { it.optString("status") == "dumped" }
        val failureCount = coverageEntries.size - successCount

        ZipOutputStream(FileOutputStream(zipFile)).use { zip ->
            zip.putTextEntry("manifest.json", createManifest(authContext, zipFile.name).toString(2))
            zip.putTextEntry("official_user_api_dump.jsonl", apiDumpText)
            zip.putTextEntry("official_user_request_audit.jsonl", auditText)
            zip.putTextEntry(
                "capture_summary.json",
                createCaptureSummary(existingApiDump, existingAudit, coverageEntries, directApiEvents.size).toString(2)
            )
            zip.putTextEntry("coverage.json", JSONObject().put("entries", JSONArray(coverageEntries)).toString(2))
        }

        return OfficialUserDataDumpResult(zipFile, successCount, failureCount)
    }

    private fun createCollectionPlan(playerId: String): List<CollectionTarget> {
        return listOf(
            CollectionTarget("registeredUsers", "/profile/get_info", JSONObject().put("player_id", playerId)),
            CollectionTarget("userHeaders", "/home/get_home", JSONObject()),
            CollectionTarget("userInventories", "/user/items/get_list", JSONObject()),
            CollectionTarget("userCards", "/user/card/get_list", JSONObject().put("search_conditions", "{}")),
            CollectionTarget("userProfileSettings", "/home/get_custom_setting", JSONObject()),
            CollectionTarget("followRelations", "/follow/get_list", JSONObject()),
            CollectionTarget("userFriendCards", "/follow/get_follower_list", JSONObject().put("order_type", 1).put("desc_order", true).put("limit", 100).put("offset", 0)),
            CollectionTarget("circle", "/circle/get_circle_top_info", JSONObject()),
            CollectionTarget("userCircleNotifications", "/circle/get_invite_and_join_info", JSONObject()),
            CollectionTarget("userGachas", "/gacha/get_series_list", JSONObject()),
            CollectionTarget("userGachas", "/gacha/get_guarantee_point_list", JSONObject()),
            CollectionTarget("userPresentBoxes", "/present_box/get_list", JSONObject().put("page", 1)),
            CollectionTarget("userArchiveCollections", "/archive/get_home", JSONObject()),
            CollectionTarget("userMissions", "/mission/get_list", JSONObject()),
            CollectionTarget("userBeginnerMissions", "/beginner_mission/get_list", JSONObject()),
            CollectionTarget("userStepUpBeginnerMissions", "/step_up_beginner_mission/get_list", JSONObject()),
            CollectionTarget("userLives", "/out_quest_live/get_quest_top", JSONObject()),
            CollectionTarget("userDailyQuestStates", "/out_quest_live/daily/get_stage_select", JSONObject()),
            CollectionTarget("userChapters", "/chapter/home", JSONObject()),
        )
    }

    private fun readLatestAuthContext(apiDumpFile: File): OfficialApiAuthContext {
        require(apiDumpFile.exists()) { "official_api_dump.jsonlがありません。先に公式ログインを1回行ってください。" }

        var latest: OfficialApiAuthContext? = null
        apiDumpFile.forEachLine(Charsets.UTF_8) { line ->
            if (line.isBlank()) return@forEachLine
            val event = runCatching { JSONObject(line) }.getOrNull() ?: return@forEachLine
            if (event.optString("request_path") != "/v1/user/login") return@forEachLine
            val response = event.optJSONObject("response")
                ?: event.optJSONObject("rest_response")?.optString("content")?.let { runCatching { JSONObject(it) }.getOrNull() }
                ?: return@forEachLine
            val sessionToken = response.optString("session_token")
            if (sessionToken.isBlank()) return@forEachLine

            val requestEnvelope = runCatching { JSONObject(event.optString("request")) }.getOrNull()
            val request = requestEnvelope?.optString("request")?.let { runCatching { JSONObject(it) }.getOrNull() }
            val playerId = request?.optString("player_id").orEmpty()
            val deviceSpecificId = request?.optString("device_specific_id").orEmpty()
            if (playerId.isBlank() || deviceSpecificId.isBlank()) return@forEachLine

            latest = OfficialApiAuthContext(
                playerId = playerId,
                deviceSpecificId = deviceSpecificId,
                sessionToken = sessionToken,
                clientVersion = event.optString("current_client_version").ifBlank { "5.1.0" },
                resVersion = event.optString("current_res_version").ifBlank { "R2606002" },
            )
        }
        return requireNotNull(latest) { "公式ログイン済みのsession_tokenをofficial_api_dump.jsonlから取得できませんでした。" }
    }

    private fun postOfficialApi(
        authContext: OfficialApiAuthContext,
        path: String,
        body: String,
    ): DirectApiResult {
        val capturedAtMs = System.currentTimeMillis()
        val event = JSONObject()
            .put("kind", "official_user_dump_direct_response")
            .put("request_path", "/v1$path")
            .put("request", JSONObject().put("request", body).toString())
            .put("current_client_version", authContext.clientVersion)
            .put("current_res_version", authContext.resVersion)
            .put("api_source", "official_direct")
            .put("captured_at_ms", capturedAtMs)

        return try {
            val connection = (URL("$apiBaseUrl$path").openConnection() as HttpURLConnection).apply {
                requestMethod = "POST"
                connectTimeout = 15000
                readTimeout = 20000
                doOutput = true
                setRequestProperty("content-type", "application/json")
                setRequestProperty("accept", "application/json")
                setRequestProperty("x-client-version", authContext.clientVersion)
                setRequestProperty("x-res-version", authContext.resVersion.substringBefore("@"))
                setRequestProperty("x-device-type", "android")
                setRequestProperty("x-device-specific-id", authContext.deviceSpecificId)
                setRequestProperty("x-api-key", apiKey)
                setRequestProperty("inspix-user-api-version", "1.0.0")
                setRequestProperty("authorization", "Bearer ${authContext.sessionToken}")
                setRequestProperty("user-agent", "inspix-android/${authContext.clientVersion}")
            }
            connection.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            val statusCode = connection.responseCode
            val responseBody = (if (statusCode in 200..399) connection.inputStream else connection.errorStream)
                ?.use { it.readBytes().toString(Charsets.UTF_8) }
                .orEmpty()
            event.put(
                "rest_response",
                JSONObject()
                    .put("content", responseBody)
                    .put("headers", headersToJson(connection.headerFields))
                    .put("status_code", statusCode)
                    .put("status_description", connection.responseMessage.orEmpty())
                    .put("body_sha256", sha256(responseBody.toByteArray(Charsets.UTF_8)))
            )
            DirectApiResult(statusCode in 200..399, statusCode, if (statusCode in 200..399) "" else responseBody, capturedAtMs, event)
        } catch (e: Exception) {
            Log.e(TAG, "official user dump request failed: $path", e)
            event.put("error", e.toString())
            DirectApiResult(false, 0, e.toString(), capturedAtMs, event)
        }
    }

    private fun createManifest(authContext: OfficialApiAuthContext, fileName: String): JSONObject {
        return JSONObject()
            .put("format_version", 1)
            .put("created_at_ms", System.currentTimeMillis())
            .put("created_at_utc", utcNow())
            .put("file_name", fileName)
            .put("player_id", authContext.playerId)
            .put("device_specific_id", authContext.deviceSpecificId)
            .put("current_client_version", authContext.clientVersion)
            .put("current_res_version", authContext.resVersion)
            .put("source", "localify_official_user_data_dump")
    }

    private fun createCaptureSummary(
        existingApiDump: File,
        existingAudit: File,
        coverageEntries: List<JSONObject>,
        directResponseCount: Int,
    ): JSONObject {
        return JSONObject()
            .put("existing_api_dump_bytes", existingApiDump.lengthOrZero())
            .put("existing_request_audit_bytes", existingAudit.lengthOrZero())
            .put("direct_response_count", directResponseCount)
            .put("coverage_total", coverageEntries.size)
            .put("coverage_dumped", coverageEntries.count { it.optString("status") == "dumped" })
            .put("coverage_missing", coverageEntries.count { it.optString("status") == "missing" })
    }

    private fun headersToJson(headers: Map<String?, List<String>>): JSONArray {
        val output = JSONArray()
        headers.forEach { (name, values) ->
            if (name != null) {
                output.put(JSONObject().put("name", name).put("values", JSONArray(values)))
            }
        }
        return output
    }

    private fun ZipOutputStream.putTextEntry(name: String, text: String) {
        putNextEntry(ZipEntry(name))
        write(text.toByteArray(Charsets.UTF_8))
        closeEntry()
    }

    private fun StringBuilder.appendRedactedJsonlFileIfExists(file: File) {
        if (file.exists()) {
            file.forEachLine(Charsets.UTF_8) { line ->
                if (line.isBlank()) return@forEachLine
                append(redactSensitiveEventLine(line)).append('\n')
            }
        }
    }

    private fun redactSensitiveEventLine(line: String): String {
        val event = runCatching { JSONObject(line) }.getOrNull() ?: return line
        redactSensitiveJson(event)
        val restResponse = event.optJSONObject("rest_response")
        val content = restResponse?.optString("content").orEmpty()
        if (content.isNotBlank()) {
            val contentJson = runCatching { JSONObject(content) }.getOrNull()
            if (contentJson != null) {
                redactSensitiveJson(contentJson)
                restResponse?.put("content", contentJson.toString())
            }
        }
        return event.toString()
    }

    private fun redactSensitiveJson(value: Any) {
        when (value) {
            is JSONObject -> {
                sensitiveKeys.forEach { value.remove(it) }
                value.keys().asSequence().toList().forEach { key ->
                    val child = value.opt(key)
                    if (child != null) redactSensitiveJson(child)
                }
            }
            is JSONArray -> {
                for (index in 0 until value.length()) {
                    val child = value.opt(index)
                    if (child != null) redactSensitiveJson(child)
                }
            }
        }
    }

    private fun File.lengthOrZero(): Long = if (exists()) length() else 0L

    private fun utcNow(): String {
        val formatter = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US)
        formatter.timeZone = TimeZone.getTimeZone("UTC")
        return formatter.format(Date())
    }

    private fun sha256(bytes: ByteArray): String {
        return MessageDigest.getInstance("SHA-256")
            .digest(bytes)
            .joinToString("") { "%02x".format(it) }
    }

    private val sensitiveKeys = setOf("session_token", "id_token", "authorization")
}

private data class OfficialApiAuthContext(
    val playerId: String,
    val deviceSpecificId: String,
    val sessionToken: String,
    val clientVersion: String,
    val resVersion: String,
)

private data class CollectionTarget(
    val category: String,
    val path: String,
    val requestBody: JSONObject,
)

private data class DirectApiResult(
    val isSuccess: Boolean,
    val statusCode: Int,
    val reason: String,
    val capturedAtMs: Long,
    val event: JSONObject,
)
