package com.shadps4.android.feature.session

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.shadps4.android.runtime.session.NativeFexSession
import kotlinx.coroutines.delay
import org.json.JSONObject

/** Native owns status/result; displaying or dismissing Compose never implies success. */
@Composable
fun SaveDataDialog(generation: Long, onVisible: (Boolean) -> Unit) {
    var model by remember(generation) { mutableStateOf<JSONObject?>(null) }
    LaunchedEffect(generation) {
        while (generation != 0L) {
            model = NativeFexSession.nativeSaveDialogSnapshot(generation)?.let { JSONObject(it) }
            delay(100)
        }
    }
    val visible = model != null
    LaunchedEffect(visible) { onVisible(visible) }
    DisposableEffect(generation) { onDispose { onVisible(false) } }
    val current = model ?: return
    val request = current.getLong("request")
    val mode = current.getInt("mode")
    val buttons = current.getInt("buttons")
    val cancel = current.getBoolean("cancel")
    val respond: (Int, Int) -> Unit = { action, selection ->
        if (NativeFexSession.nativeSaveDialogRespond(generation, request, action, selection)) model = null
    }
    AlertDialog(
        onDismissRequest = { if (cancel) respond(0, -1) },
        title = { Text("Saved data") },
        text = {
            Column(Modifier.heightIn(max = 360.dp).verticalScroll(rememberScrollState())) {
                Text(current.getString("text"))
                if (mode == 5) LinearProgressIndicator(progress = { current.getInt("progress") / 100f })
                if (mode == 1) {
                    val items = current.getJSONArray("items")
                    for (i in 0 until items.length()) TextButton(onClick = { respond(1, i) }) { Text(items.getString(i)) }
                }
            }
        },
        confirmButton = {
            if (mode != 1 && buttons != 2) TextButton(onClick = { respond(1, -1) }) { Text(if (buttons == 1) "Yes" else "OK") }
        },
        dismissButton = {
            if (buttons == 1) TextButton(onClick = { respond(2, -1) }) { Text("No") }
            if (cancel) TextButton(onClick = { respond(0, -1) }) { Text("Cancel") }
        },
    )
}
