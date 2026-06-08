package io.github.chocolzs.linkura.localify.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import io.github.chocolzs.linkura.localify.R
import io.github.chocolzs.linkura.localify.mainUtils.MockItemCatalogRepository
import io.github.chocolzs.linkura.localify.models.MockItemCatalogItem

@Composable
fun MockItemNumEditor(
    overrides: Map<Int, Int>,
    onOverrideChanged: (Int, Int?) -> Unit,
    modifier: Modifier = Modifier
) {
    var itemIdText by remember { mutableStateOf("") }
    var itemNumText by remember { mutableStateOf("") }
    var searchText by remember { mutableStateOf("") }
    var catalogItems by remember { mutableStateOf<List<MockItemCatalogItem>>(emptyList()) }
    val context = LocalContext.current
    val keyboardOptionsNumber = remember {
        KeyboardOptions(keyboardType = KeyboardType.Number)
    }
    val itemId = itemIdText.toIntOrNull()
    val itemNum = itemNumText.toIntOrNull()
    val searchResults = remember(searchText, catalogItems) {
        val query = searchText.trim()
        if (query.isEmpty()) {
            emptyList()
        } else {
            catalogItems.asSequence()
                .filter { item ->
                    item.itemId.toString().contains(query) ||
                        item.name.contains(query, ignoreCase = true) ||
                        item.nameFurigana.contains(query, ignoreCase = true)
                }
                .take(20)
                .toList()
        }
    }

    LaunchedEffect(context) {
        catalogItems = MockItemCatalogRepository.load(context)
    }

    Column(
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Text(
            text = stringResource(R.string.mock_item_num_overrides),
            style = MaterialTheme.typography.titleSmall
        )
        GakuTextInput(
            value = searchText,
            onValueChange = { searchText = it },
            modifier = Modifier.fillMaxWidth(),
            label = { Text(stringResource(R.string.mock_item_search)) }
        )
        searchResults.forEach { item ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = item.name,
                        style = MaterialTheme.typography.bodySmall
                    )
                    Text(
                        text = stringResource(
                            R.string.mock_item_search_result_detail,
                            item.itemId,
                            item.defaultItemNum
                        ),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
                    )
                }
                GakuButton(
                    modifier = Modifier.height(32.dp),
                    text = stringResource(R.string.select_mock_item),
                    onClick = {
                        itemIdText = item.itemId.toString()
                        if (itemNumText.isEmpty()) {
                            itemNumText = item.defaultItemNum.toString()
                        }
                    }
                )
            }
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            GakuTextInput(
                value = itemIdText,
                onValueChange = { itemIdText = it.filter(Char::isDigit) },
                modifier = Modifier.weight(1f),
                label = { Text(stringResource(R.string.mock_item_id)) },
                keyboardOptions = keyboardOptionsNumber
            )
            GakuTextInput(
                value = itemNumText,
                onValueChange = { itemNumText = it.filter(Char::isDigit) },
                modifier = Modifier.weight(1f),
                label = { Text(stringResource(R.string.mock_item_num)) },
                keyboardOptions = keyboardOptionsNumber
            )
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            GakuButton(
                modifier = Modifier
                    .weight(1f)
                    .height(36.dp),
                text = stringResource(R.string.apply_mock_item_num),
                enabled = itemId != null && itemNum != null,
                onClick = {
                    if (itemId != null && itemNum != null) {
                        onOverrideChanged(itemId, itemNum)
                    }
                }
            )
            GakuButton(
                modifier = Modifier
                    .weight(1f)
                    .height(36.dp),
                text = stringResource(R.string.remove_mock_item_num),
                enabled = itemId != null,
                onClick = {
                    if (itemId != null) {
                        onOverrideChanged(itemId, null)
                    }
                }
            )
        }
        if (overrides.isNotEmpty()) {
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                overrides.entries.sortedBy { it.key }.forEach { entry ->
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Text(
                            text = stringResource(R.string.mock_item_override_row_item, entry.key),
                            style = MaterialTheme.typography.bodySmall
                        )
                        Text(
                            text = entry.value.toString(),
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                }
            }
        }
    }
}
