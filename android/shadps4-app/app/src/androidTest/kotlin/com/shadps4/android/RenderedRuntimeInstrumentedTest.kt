package com.shadps4.android

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.runtime.input.NativePad
import com.shadps4.android.runtime.input.NativePadBridge
import com.shadps4.android.runtime.session.AndroidTurnip
import com.shadps4.android.runtime.session.ManagedSession
import com.shadps4.android.runtime.session.NativeFexSession
import java.io.File
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Optional real-content boundary evidence. No copyrighted assets are packaged. */
@RunWith(AndroidJUnit4::class)
class RenderedRuntimeInstrumentedTest {
    @Test fun syntheticVideoOutValidatesStackArgumentsFaultsAndRestarts() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val root = File(context.filesDir, "validation/videoout-${System.nanoTime()}").apply { mkdirs() }
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").absolutePath))
        val paths = AndroidTurnip.prepare(context)
        val identity = NativeFexSession.nativeIdentity()
        var previous = 0L
        try {
            listOf("videoout", "gpu-flip", "videoout-bad", "gpu-flip", "videoout-format", "gpu-flip").forEachIndexed { round, name ->
                val entry = File(root, "eboot.bin")
                instrumentation.context.assets.open("$name.elf").use { input ->
                    entry.outputStream().use { input.copyTo(it) }
                }
                RuntimeTestSurface(instrumentation).use { window ->
                    val generation = NativeFexSession.nativeStartRenderedExecutable("videoout-contract",
                        entry.path, window.surface, paths.hooks, paths.driver)
                    assertTrue(generation > previous)
                    previous = generation
                    try {
                        assertTrue(NativeFexSession.nativePlatformReady(generation))
                        val outcome = NativeFexSession.nativeWaitTerminal(generation, 15000)
                        val detail = NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                        android.util.Log.i("RenderedRuntimeAcceptance", "synthetic round=${round + 1} case=$name gen=$generation outcome=$outcome $identity $detail")
                        assertEquals(detail, if (name == "videoout" || name == "gpu-flip") NativeFexSession.Outcome.RETURNED
                            else NativeFexSession.Outcome.FAULTED, outcome)
                        assertTrue(detail, detail.contains("graphics=ready"))
                        if (name == "videoout" || name == "gpu-flip") assertTrue(detail, detail.startsWith("guest return=51966"))
                        else assertTrue(detail, detail.contains("import=i6-sR91Wt-4#"))
                        if (name == "gpu-flip") {
                            val presented = Regex("guest_presents=(\\d+)").find(detail)?.groupValues?.get(1)?.toInt() ?: 0
                            assertTrue("No four actual guest frame presents: $detail", presented >= 4)
                        }
                        assertEquals(identity, NativeFexSession.nativeIdentity())
                    } finally {
                        NativeFexSession.nativeRequestStop(generation, 1000)
                    }
                }
            }
        } finally {
            if (NativeFexSession.nativeCurrentGeneration() == 0L) root.deleteRecursively()
        }
    }

    @Test fun realContentUsesSessionRendererAcrossThreeRestarts() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val relative = InstrumentationRegistry.getArguments().getString("contentRelativePath")
        assumeTrue("NOT_RUN: real content path not supplied", relative != null)
        val root = File(context.filesDir, relative!!).canonicalFile
        require(root.toPath().startsWith(context.filesDir.canonicalFile.toPath()))
        val entry = File(root, "eboot.bin")
        require(entry.isFile)
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir, "host").absolutePath))
        InstrumentationRegistry.getArguments().getString("debugGate")?.let { name ->
            require(name.matches(Regex("[a-zA-Z0-9-]+")))
            val release = File(context.cacheDir, "$name-continue")
            File(context.cacheDir, "$name-ready").writeText("ready")
            val deadline = android.os.SystemClock.uptimeMillis() + 120000
            try {
                while (!release.exists() && android.os.SystemClock.uptimeMillis() < deadline)
                    android.os.SystemClock.sleep(20)
                check(release.exists()) { "debug gate timed out" }
            } finally {
                release.delete()
                File(context.cacheDir, "$name-ready").delete()
            }
        }
        val paths = AndroidTurnip.prepare(context)
        val identity = NativeFexSession.nativeIdentity()
        var previous = 0L
        repeat(3) { round ->
            RuntimeTestSurface(instrumentation).use { window ->
                val generation = NativeFexSession.nativeStartRenderedExecutable("real-content-boundary",
                    entry.path, window.surface, paths.hooks, paths.driver)
                assertTrue("new generation", generation > previous)
                previous = generation
                try {
                    assertEquals("prepared", NativeFexSession.WaitPhase.REACHED_TARGET,
                        NativeFexSession.nativeWaitPhase(generation, NativeFexSession.PhaseOrdinal.READY, 10000))
                    assertEquals("guest waits for input/platform", NativeFexSession.WaitPhase.TIMEOUT,
                        NativeFexSession.nativeWaitPhase(generation, NativeFexSession.PhaseOrdinal.RUNNING, 20))
                    instrumentation.runOnMainSync {
                        ManagedSession.beginGeneration(generation)
                        NativePadBridge.begin(context, generation)
                    }
                    assertTrue(NativeFexSession.nativePlatformReady(generation))
                    val outcome = NativeFexSession.nativeWaitTerminal(generation, 45000)
                    val detail = NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                    android.util.Log.i("RenderedRuntimeAcceptance", "round=${round + 1} gen=$generation outcome=$outcome $identity $detail")
                    assertEquals(detail, NativeFexSession.Outcome.FAULTED, outcome)
                    assertTrue(detail, detail.contains("graphics=ready"))
                    assertFalse(detail, detail.contains("import=Up36PTk687E#"))
                    assertEquals(identity, NativeFexSession.nativeIdentity())
                } finally {
                    NativeFexSession.nativeRequestStop(generation, 1000)
                    instrumentation.runOnMainSync { NativePadBridge.end(generation) }
                }
            }
        }
    }
}
