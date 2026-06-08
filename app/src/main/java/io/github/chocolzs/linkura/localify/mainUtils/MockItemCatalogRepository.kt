package io.github.chocolzs.linkura.localify.mainUtils

import android.content.Context
import io.github.chocolzs.linkura.localify.models.MockItemCatalogItem
import kotlinx.serialization.json.Json

object MockItemCatalogRepository {
    private val json = Json { ignoreUnknownKeys = true }
    private var cache: List<MockItemCatalogItem>? = null

    fun load(context: Context): List<MockItemCatalogItem> {
        cache?.let { return it }
        val items = context.assets.open("mock_items.json").bufferedReader().use { reader ->
            json.decodeFromString<List<MockItemCatalogItem>>(reader.readText())
        }
        cache = items
        return items
    }
}
