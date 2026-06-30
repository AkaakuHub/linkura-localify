package io.github.chocolzs.linkura.localify

import android.content.Context
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
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

data class OfficialUserDataDumpProgress(
    val completedCount: Int,
    val totalCount: Int,
) {
    val percent: Int
        get() = if (totalCount == 0) 0 else (completedCount * 100 / totalCount).coerceIn(0, 100)
}

object OfficialUserDataDumpExporter {
    private const val dumpFormatVersion = 2
    private const val apiBaseUrl = "https://api.link-like-lovelive.app/v1"
    private const val apiKey = "4e769efa67d8f54be0b67e8f70ccb23d513a3c841191b6b2ba45ffc6fb498068"

    fun collectAndExport(
        context: Context,
        onProgress: ((OfficialUserDataDumpProgress) -> Unit)? = null,
    ): OfficialUserDataDumpResult {
        val filesDir = context.filesDir
        val existingApiDump = File(filesDir, "official_api_dump.jsonl")
        val existingAudit = File(filesDir, "official_request_audit.jsonl")
        val authContext = readLatestAuthContext(existingApiDump)

        val directApiEvents = mutableListOf<String>()
        val directAuditEvents = mutableListOf<String>()
        val coverageEntries = mutableListOf<JSONObject>()

        val plan = createCollectionPlan(authContext.playerId)
        var completedCount = 0
        var totalCount = plan.size

        fun reportProgress() {
            onProgress?.invoke(OfficialUserDataDumpProgress(completedCount, totalCount))
        }

        fun completeTarget() {
            completedCount += 1
            reportProgress()
        }

        for (target in plan) {
            val requestBody = target.requestBody.toString()
            val startedAtMs = System.currentTimeMillis()
            directAuditEvents += JSONObject()
                .put("kind", "official_user_dump_direct_request")
                .put("target", "/v1${target.path}")
                .put("detail", createRequestDetail(target, requestBody))
                .put("current_client_version", authContext.clientVersion)
                .put("current_res_version", authContext.resVersion)
                .put("captured_at_ms", startedAtMs)
                .toString()

            val result = postOfficialApi(authContext, target, requestBody)
            directApiEvents += result.event.toString()
            coverageEntries += JSONObject()
                .put("category", target.category)
                .put("source_endpoint", "/v1${target.path}")
                .put("request", target.requestBody)
                .put("query_parameters", target.queryParameters)
                .put("status", if (result.isSuccess) "dumped" else "missing")
                .put("status_code", result.statusCode)
                .put("reason", result.reason)
                .put("captured_at_ms", result.capturedAtMs)
            completeTarget()
            if (target.path == "/user/card/get_list" && result.isSuccess) {
                val cardIds = extractUserCardIds(result.event)
                totalCount += cardIds.size
                reportProgress()
                collectUserCardDetails(authContext, cardIds, directApiEvents, directAuditEvents, coverageEntries, ::completeTarget)
            }
            if (target.path == "/circle/get_circle_top_info" && result.isSuccess) {
                val dependentTargets = extractCircleDependentTargets(result.event)
                totalCount += dependentTargets.size
                reportProgress()
                collectDependentTargets(authContext, dependentTargets, directApiEvents, directAuditEvents, coverageEntries, ::completeTarget)
            }
            if (target.path == "/out_quest_live/get_quest_top" && result.isSuccess) {
                val dependentTargets = extractQuestTopDependentTargets(result.event)
                totalCount += dependentTargets.size
                reportProgress()
                collectDependentTargets(authContext, dependentTargets, directApiEvents, directAuditEvents, coverageEntries, ::completeTarget)
            }
            if (target.path == "/chapter/home" && result.isSuccess) {
                val dependentTargets = extractChapterCategoryInfoTargets(result.event)
                totalCount += dependentTargets.size
                reportProgress()
                collectDependentTargets(authContext, dependentTargets, directApiEvents, directAuditEvents, coverageEntries, ::completeTarget)
            }
            if (target.path == "/select_ticket_exchange/get_list" && result.isSuccess) {
                val dependentTargets = extractSelectTicketExchangeCardTargets(result.event)
                totalCount += dependentTargets.size
                reportProgress()
                collectDependentTargets(authContext, dependentTargets, directApiEvents, directAuditEvents, coverageEntries, ::completeTarget)
            }
            if (target.path == "/rhythm_game/home" && result.isSuccess) {
                val dependentTargets = extractRhythmGameGrandPrixTargets(result.event)
                totalCount += dependentTargets.size
                reportProgress()
                collectDependentTargets(authContext, dependentTargets, directApiEvents, directAuditEvents, coverageEntries, ::completeTarget)
            }
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
            CollectionTarget("userDecks", "/user/deck/get_list", JSONObject()),
            CollectionTarget("userProfileSettings", "/home/get_custom_setting", JSONObject()),
            CollectionTarget("userProfileSettings", "/home/get_wallpaper_setting", JSONObject()),
            CollectionTarget("userProfileDesigns", "/profile/get_my_design_card_list", JSONObject()),
            CollectionTarget("userProfileDesigns", "/profile/get_my_design_icon_list", JSONObject()),
            CollectionTarget("userProfileSettings", "/profile/get_mute_list", JSONObject()),
            CollectionTarget("userStatuses", "/profile/get_fan_level_info", JSONObject().put("player_id", playerId)),
            CollectionTarget("followRelations", "/follow/get_list", JSONObject()),
            CollectionTarget("userFriendCards", "/follow/get_follower_list", JSONObject().put("order_type", 1).put("desc_order", true).put("limit", 100).put("offset", 0)),
            CollectionTarget("userSocials", "/follow/live_chat_group_list", JSONObject()),
            CollectionTarget("circle", "/circle/get_circle_top_info", JSONObject()),
            CollectionTarget("userCircleNotifications", "/circle/get_invite_list", JSONObject()),
            CollectionTarget("userGachas", "/gacha/get_series_list", JSONObject()),
            CollectionTarget("userGachas", "/gacha/get_history", JSONObject()),
            CollectionTarget("userGachas", "/select_ticket_exchange/get_list", JSONObject()),
            CollectionTarget("userPresentBoxes", "/present_box/get_list", JSONObject().put("page", 1)),
            CollectionTarget("userPresentBoxes", "/present_box/get_history", JSONObject().put("page", 1)),
            CollectionTarget("userArchiveCollections", "/archive/get_home", JSONObject()),
            CollectionTarget("userActivityRecords", "/activity_record/get_top", JSONObject()),
            CollectionTarget("userCollections", "/collection/get_gallary_list", JSONObject()),
            CollectionTarget("userCollections", "/collection/get_music_list", JSONObject()),
            CollectionTarget("userCollections", "/collection/get_sticker_list", JSONObject()),
            CollectionTarget("userMissions", "/mission/get_list", JSONObject()),
            CollectionTarget("userBeginnerMissions", "/beginner_mission/get_list", JSONObject()),
            CollectionTarget("userStepUpBeginnerMissions", "/step_up_beginner_mission/get_list", JSONObject()),
            CollectionTarget("userShops", "/shop/get_list", JSONObject()),
            CollectionTarget("userShops", "/petal_exchange/get_list", JSONObject()),
            CollectionTarget("userShops", "/gp_prize_exchange/get_list", JSONObject()),
            CollectionTarget("userShops", "/item_exchange/get_list_new", JSONObject()),
            CollectionTarget("userShops", "/item_exchange/get_limit_break_material_convert_list", JSONObject()),
            CollectionTarget("userShops", "/sticker_exchange/get_list", JSONObject()),
            CollectionTarget("userShops", "/sisca_store/get_list", JSONObject()),
            CollectionTarget("userLives", "/out_quest_live/get_quest_top", JSONObject()),
            CollectionTarget("userLives", "/out_quest_live/get_stamina_recovery_info", JSONObject()),
            CollectionTarget("userLives", "/out_quest_live/grade/get_quest_list", JSONObject()),
            CollectionTarget("userDailyQuestStates", "/out_quest_live/daily/get_stage_select", JSONObject()),
            CollectionTarget("userDailyQuestStates", "/out_quest_live/daily/get_recovery_challenge_count", JSONObject()),
            CollectionTarget("userLives", "/out_quest_live/music_learning/get_music_select", JSONObject()),
            CollectionTarget("userRhythmGame", "/rhythm_game/home", JSONObject()),
            CollectionTarget("userChapters", "/chapter/home", JSONObject()),
        )
    }

    private fun collectUserCardDetails(
        authContext: OfficialApiAuthContext,
        cardIds: List<String>,
        directApiEvents: MutableList<String>,
        directAuditEvents: MutableList<String>,
        coverageEntries: MutableList<JSONObject>,
        onTargetCompleted: () -> Unit,
    ) {
        for (dCardDatasId in cardIds) {
            val target = CollectionTarget("userCardDetails", "/user/card/get_detail", JSONObject().put("d_card_datas_id", dCardDatasId))
            collectDependentTarget(authContext, target, directApiEvents, directAuditEvents, coverageEntries, onTargetCompleted)
        }
    }

    private fun collectDependentTargets(
        authContext: OfficialApiAuthContext,
        targets: List<CollectionTarget>,
        directApiEvents: MutableList<String>,
        directAuditEvents: MutableList<String>,
        coverageEntries: MutableList<JSONObject>,
        onTargetCompleted: () -> Unit,
    ) {
        for (target in targets) {
            collectDependentTarget(authContext, target, directApiEvents, directAuditEvents, coverageEntries, onTargetCompleted)
        }
    }

    private fun collectDependentTarget(
        authContext: OfficialApiAuthContext,
        target: CollectionTarget,
        directApiEvents: MutableList<String>,
        directAuditEvents: MutableList<String>,
        coverageEntries: MutableList<JSONObject>,
        onTargetCompleted: () -> Unit,
    ) {
        val requestBody = target.requestBody.toString()
        val startedAtMs = System.currentTimeMillis()
        directAuditEvents += JSONObject()
            .put("kind", "official_user_dump_direct_request")
            .put("target", "/v1${target.path}")
            .put("detail", createRequestDetail(target, requestBody))
            .put("current_client_version", authContext.clientVersion)
            .put("current_res_version", authContext.resVersion)
            .put("captured_at_ms", startedAtMs)
            .toString()
        val result = postOfficialApi(authContext, target, requestBody)
        directApiEvents += result.event.toString()
        coverageEntries += JSONObject()
            .put("category", target.category)
            .put("source_endpoint", "/v1${target.path}")
            .put("request", target.requestBody)
            .put("query_parameters", target.queryParameters)
            .put("status", if (result.isSuccess) "dumped" else "missing")
            .put("status_code", result.statusCode)
            .put("reason", result.reason)
            .put("captured_at_ms", result.capturedAtMs)
        onTargetCompleted()
    }

    private fun extractCircleSearchGuildKey(circleTopInfoEvent: JSONObject): String? {
        val response = extractResponse(circleTopInfoEvent) ?: return null
        return response.optJSONObject("circle_info")
            ?.optString("search_guild_key")
            ?.takeIf { it.isNotBlank() }
    }

    private fun extractUserCardIds(cardListEvent: JSONObject): List<String> {
        val response = extractResponse(cardListEvent)
            ?: return emptyList()
        val cards = response.optJSONArray("user_card_data_list") ?: return emptyList()
        val cardIds = mutableListOf<String>()
        for (index in 0 until cards.length()) {
            val cardId = cards.optJSONObject(index)?.optString("d_card_datas_id").orEmpty()
            if (cardId.isNotBlank()) cardIds += cardId
        }
        return cardIds.distinct()
    }

    private fun extractCircleDependentTargets(circleTopInfoEvent: JSONObject): List<CollectionTarget> {
        val searchGuildKey = extractCircleSearchGuildKey(circleTopInfoEvent) ?: return emptyList()
        val searchRequest = JSONObject().put("search_guild_key", searchGuildKey)
        return listOf(
            CollectionTarget("circle", "/circle/get_detail", searchRequest),
            CollectionTarget("circle", "/circle/get_info", searchRequest),
            CollectionTarget("userCircleNotifications", "/circle/get_invite_and_join_info", searchRequest),
            CollectionTarget(
                "circleChatLogs",
                "/circle/get_chat_log_list",
                JSONObject(),
                "GET",
                JSONObject()
                    .put("diff_order_id", 0)
                    .put("is_item_request", true)
                    .put("already_read_order_id", 0)
            ),
        )
    }

    private fun extractQuestTopDependentTargets(questTopEvent: JSONObject): List<CollectionTarget> {
        val response = extractResponse(questTopEvent) ?: return emptyList()
        val targets = mutableListOf<CollectionTarget>()
        val grandPrixId = response.optInt("grand_prix_id", 0)
        if (grandPrixId > 0) {
            val request = JSONObject().put("grand_prix_id", grandPrixId)
            targets += CollectionTarget("userGrandPrixes", "/out_quest_live/grand_prix/get_top_info", request)
            targets += CollectionTarget("userGrandPrixes", "/out_quest_live/grand_prix/get_stage_select", request)
            targets += CollectionTarget("userGrandPrixes", "/out_quest_live/grand_prix/get_history", request)
        }
        val gradeChallengeSeasonId = response.optInt("grade_challenge_season_id", 0)
        if (gradeChallengeSeasonId > 0) {
            targets += CollectionTarget(
                "userGradeChallengeProgress",
                "/out_quest_live/grade_challenge/get_quest_list",
                JSONObject().put("grade_chal_season_id", gradeChallengeSeasonId)
            )
        }
        val standardAreaList = response.optJSONObject("standard_area_select")?.optJSONArray("area_list")
        if (standardAreaList != null) {
            val areaIds = mutableListOf<Int>()
            for (index in 0 until standardAreaList.length()) {
                val areaId = standardAreaList.optJSONObject(index)?.optInt("area_id", 0) ?: 0
                if (areaId > 0) areaIds += areaId
            }
            for (areaId in areaIds.distinct()) {
                targets += CollectionTarget(
                    "userStandardQuestProgress",
                    "/out_quest_live/standard/get_stage_select",
                    JSONObject().put("area_id", areaId)
                )
            }
        }
        return targets
    }

    private fun extractChapterCategoryInfoTargets(chapterHomeEvent: JSONObject): List<CollectionTarget> {
        val response = extractResponse(chapterHomeEvent) ?: return emptyList()
        val categoryList = response.optJSONArray("category_list") ?: return emptyList()
        val categoryIds = mutableListOf<Int>()
        for (index in 0 until categoryList.length()) {
            val categoryId = categoryList.optJSONObject(index)?.optInt("category_id", 0) ?: 0
            if (categoryId > 0) categoryIds += categoryId
        }
        val chapterId = response.optInt("chapter_id", 0)
        val targets = categoryIds.distinct().map { categoryId ->
            CollectionTarget("userChapterMissions", "/chapter/category_info", JSONObject().put("category_id", categoryId))
        }.toMutableList()
        if (chapterId > 0) {
            targets += CollectionTarget("userChapters", "/chapter/get_point_rewards", JSONObject().put("chapter_id", chapterId))
        }
        return targets
    }

    private fun extractSelectTicketExchangeCardTargets(selectTicketExchangeEvent: JSONObject): List<CollectionTarget> {
        val response = extractResponse(selectTicketExchangeEvent) ?: return emptyList()
        val selectTicketExchangeList = response.optJSONArray("select_ticket_exchange_list") ?: return emptyList()
        val selectTicketSeriesIds = mutableListOf<Int>()
        for (index in 0 until selectTicketExchangeList.length()) {
            val seriesId = selectTicketExchangeList.optJSONObject(index)?.optInt("select_ticket_series_id", 0) ?: 0
            if (seriesId > 0) selectTicketSeriesIds += seriesId
        }
        return selectTicketSeriesIds.distinct().map { seriesId ->
            CollectionTarget(
                "userGachaExchangeCardHaving",
                "/gacha/get_exchange_card_having_list",
                JSONObject().put("exchange_type", 2).put("exchange_id", seriesId)
            )
        }
    }

    private fun extractRhythmGameGrandPrixTargets(rhythmGameHomeEvent: JSONObject): List<CollectionTarget> {
        val response = extractResponse(rhythmGameHomeEvent) ?: return emptyList()
        val grandPrixId = response.optInt("grand_prix_id", 0)
        if (grandPrixId <= 0) return emptyList()
        return listOf(
            CollectionTarget(
                "userRhythmGameGrandPrix",
                "/rhythm_game_grand_prix/top",
                JSONObject().put("grand_prix_id", grandPrixId)
            )
        )
    }

    private fun extractResponse(event: JSONObject): JSONObject? {
        return event.optJSONObject("response")
            ?: event.optJSONObject("rest_response")?.optString("content")?.let { runCatching { JSONObject(it) }.getOrNull() }
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
        target: CollectionTarget,
        body: String,
    ): DirectApiResult {
        val path = target.path
        val capturedAtMs = System.currentTimeMillis()
        val event = JSONObject()
            .put("kind", "official_user_dump_direct_response")
            .put("request_path", "/v1$path")
            .put("request", JSONObject().put("request", body).toString())
            .put("current_client_version", authContext.clientVersion)
            .put("current_res_version", authContext.resVersion)
            .put("api_source", "official_direct")
            .put("query_parameters", target.queryParameters)
            .put("captured_at_ms", capturedAtMs)

        return try {
            val connection = (URL(buildOfficialApiUrl(path, target.queryParameters)).openConnection() as HttpURLConnection).apply {
                requestMethod = target.method
                connectTimeout = 15000
                readTimeout = 20000
                doOutput = target.method != "GET"
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
            if (target.method != "GET") {
                connection.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            }
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

    private fun createRequestDetail(target: CollectionTarget, requestBody: String): JSONObject {
        return JSONObject()
            .put("request", requestBody)
            .put("query_parameters", target.queryParameters)
    }

    private fun buildOfficialApiUrl(path: String, queryParameters: JSONObject): String {
        val query = queryParameters.keys().asSequence().joinToString("&") { key ->
            "${urlEncode(key)}=${urlEncode(queryParameters.opt(key).toString())}"
        }
        if (query.isBlank()) return "$apiBaseUrl$path"
        return "$apiBaseUrl$path?$query"
    }

    private fun urlEncode(value: String): String {
        return URLEncoder.encode(value, "UTF-8")
    }

    private fun createManifest(authContext: OfficialApiAuthContext, fileName: String): JSONObject {
        return JSONObject()
            .put("format_name", "localify_official_user_data_dump")
            .put("format_version", dumpFormatVersion)
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
    val method: String = "POST",
    val queryParameters: JSONObject = JSONObject(),
)

private data class DirectApiResult(
    val isSuccess: Boolean,
    val statusCode: Int,
    val reason: String,
    val capturedAtMs: Long,
    val event: JSONObject,
)
