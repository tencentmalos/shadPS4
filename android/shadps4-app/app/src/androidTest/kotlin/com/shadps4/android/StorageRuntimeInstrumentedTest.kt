package com.shadps4.android

import android.content.Intent
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.junit.Rule
import androidx.compose.ui.platform.ComposeView
import androidx.compose.material3.MaterialTheme
import com.shadps4.android.feature.session.SaveDataDialog
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.input.NativePad
import com.shadps4.android.runtime.session.NativeFexSession
import java.io.File
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class StorageRuntimeInstrumentedTest {
    @get:Rule val compose = createEmptyComposeRule()
    @Test fun persistentGuestStorageAndDialog() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val stage = InstrumentationRegistry.getArguments().getString("storageStage", "write")
        val root = File(context.filesDir, "validation/production-save-contract").apply { mkdirs() }
        val save = File(context.filesDir, "host/home/1000/savedata/CUSA99991")
        val ownership = File(root,"owned-save")
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir,"host").absolutePath))
        if (stage == "write") {
            // Never overwrite pre-existing saves, even for this synthetic title.
            assertFalse("Synthetic title already has data: $save",save.exists())
            ownership.writeText("CUSA99991 production storage test")
        } else assertTrue("Run storageStage=write first",ownership.isFile)
        val meta = File(root,"sce_sys/param.sfo").apply { parentFile!!.mkdirs() }
        instrumentation.context.assets.open("sce_sys/param.sfo").use { input -> meta.outputStream().use { input.copyTo(it) } }
        if (stage == "read") {
            // Simulate an installed game update: stable title/save identity,
            // changed APP_VER, a fresh process and replaced executable content.
            val bytes=meta.readBytes()
            val old="01.00\u0000".toByteArray()
            val offset=(0..bytes.size-old.size).first { i -> old.indices.all { bytes[i+it]==old[it] } }
            "01.08\u0000".toByteArray().copyInto(bytes,offset)
            meta.writeBytes(bytes)
        }
        val names = if (stage == "write") listOf("storage", "storage-read", "save-dialog") else listOf("storage-read", "save-dialog")
        try {
            for(name in names) {
                val entry = File(root,"eboot.bin")
                instrumentation.context.assets.open("$name.elf").use { input -> entry.outputStream().use { input.copyTo(it) } }
                val gen=NativeFexSession.nativeStartExecutable("storage-contract",entry.path)
                assertTrue(gen>0)
                try {
                    if(name=="save-dialog") {
                        var request: JSONObject?=null
                        val deadline=System.nanoTime()+10_000_000_000L
                        while(request==null && System.nanoTime()<deadline) {
                            request=NativeFexSession.nativeSaveDialogSnapshot(gen)?.let { JSONObject(it) }
                            if(request==null) Thread.sleep(10)
                        }
                        assertNotNull(NativeFexSession.nativeTerminalDetail(gen),request)
                        val id=request!!.getLong("request")
                        assertFalse(NativeFexSession.nativeSaveDialogRespond(gen+1,id,0,-1))
                        assertFalse(NativeFexSession.nativeSaveDialogRespond(gen,id+1,0,-1))
                        val activity=instrumentation.startActivitySync(Intent(context,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                        try {
                            instrumentation.runOnMainSync {
                                val compose=ComposeView(activity)
                                activity.setContentView(compose)
                                compose.setContent { MaterialTheme { SaveDataDialog(gen) {} } }
                            }
                            compose.waitUntil(10000) {
                                compose.onAllNodesWithText("Cancel").fetchSemanticsNodes().isNotEmpty()
                            }
                            compose.onNodeWithText("Cancel").performClick()

                        } finally {
                            instrumentation.runOnMainSync { activity.finish() }
                            instrumentation.waitForIdleSync()
                        }
                        assertFalse(NativeFexSession.nativeSaveDialogRespond(gen,id,0,-1))
                    }
                    val outcome=NativeFexSession.nativeWaitTerminal(gen,15000)
                    val detail=NativeFexSession.nativeTerminalDetail(gen).orEmpty()
                    android.util.Log.i("StorageRuntimeAcceptance","stage=$stage case=$name gen=$gen outcome=$outcome ${NativeFexSession.nativeIdentity()} $detail")
                    assertEquals(detail,NativeFexSession.Outcome.RETURNED,outcome)
                    assertTrue(detail,detail.startsWith("guest return=51966"))
                } finally { NativeFexSession.nativeRequestStop(gen,1000) }
            }
            assertTrue(File(save,"production-contract/roundtrip.bin").isFile)
            assertFalse(File(save,"production-contract/sce_sys/corrupted").exists())
            if(stage=="read") {
                assertEquals("CUSA99991 production storage test",ownership.readText())
                assertTrue(save.deleteRecursively())
                root.deleteRecursively()
            }
        } finally { assertEquals(0L,NativeFexSession.nativeCurrentGeneration()) }
    }
}
