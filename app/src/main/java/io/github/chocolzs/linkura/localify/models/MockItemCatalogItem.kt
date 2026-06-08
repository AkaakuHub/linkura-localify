package io.github.chocolzs.linkura.localify.models

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class MockItemCatalogItem(
    @SerialName("itemId")
    val itemId: Int,
    @SerialName("itemType")
    val itemType: Int,
    @SerialName("itemCategory")
    val itemCategory: Int,
    val rarity: Int,
    val name: String,
    val nameFurigana: String,
    @SerialName("defaultItemNum")
    val defaultItemNum: Int,
    @SerialName("resourceFileName")
    val resourceFileName: String
)
