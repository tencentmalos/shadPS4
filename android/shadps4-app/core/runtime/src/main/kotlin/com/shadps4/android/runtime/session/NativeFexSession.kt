package com.shadps4.android.runtime.session

/**
 * JNI bridge to libshadps4_fex_session.so and the native ARM64 host services.
 *
 * The production path executes PS4 guest code through FEX and the host Module/Linker/VM/HLE
 * runtime. Rendered sessions require a live Surface and verified bionic Turnip. Game compatibility
 * is reported by actual terminal outcomes; nativeStart remains the explicit CPU smoke entry.
 *
 * All lifecycle logic lives in the native SessionCore (src/core/host_runtime), which is unit-tested
 * on the host. This object is a typed wrapper: every call is generation-scoped, and the native side
 * surfaces a typed terminal outcome so a guest fault is never reported as a clean exit.
 */
object NativeFexSession {
    init {
        System.loadLibrary("shadps4_fex_session")
    }

    /** page_size / pid / uid of the app process. */
    external fun nativeSaveDialogSnapshot(generation: Long): String?
    external fun nativeSaveDialogRespond(generation: Long, request: Long, action: Int, selection: Int): Boolean

    external fun nativeIdentity(): String

    /**
     * Start a session. Returns the new generation (> 0), or 0 if a session is already running or the
     * owner thread could not be spawned. `iterations` bounds the smoke loop (<= 0 = large default).
     */
    external fun nativeStart(contentId: String?, iterations: Long): Long

    /** Start the production Module/Linker/VM/FEX runtime for installed content. */
    external fun nativeStartExecutable(contentId: String, executablePath: String): Long

    external fun nativeStartRenderedExecutable(contentId: String, executablePath: String,
        surface: android.view.Surface, hookDirectory: String, driverDirectory: String): Long
    external fun nativePlatformReady(generation: Long): Boolean

    /** Request a stop of [generation]. Returns a [StopResult] ordinal. */
    external fun nativeRequestStop(generation: Long, timeoutMs: Long): Int

    /** Wait until [generation] reaches [targetPhase] (or later). Returns a [WaitPhaseResult] ordinal. */
    external fun nativeWaitPhase(generation: Long, targetPhase: Int, deadlineMs: Long): Int

    /** Wait for a terminal. Returns a [RunOutcome] ordinal, or -1 on timeout (session still owned). */
    external fun nativeWaitTerminal(generation: Long, deadlineMs: Long): Int

    /** guest_cpu ErrorCategory ordinal of the terminal, or 0 (None). */
    external fun nativeTerminalErrorCategory(generation: Long): Int

    /** Structured terminal detail, or null. Not a user-facing message. */
    external fun nativeTerminalDetail(generation: Long): String?

    /** The current live generation, or 0 when idle/terminated (a killed process reports 0). */
    external fun nativeCurrentGeneration(): Long

    /** [Phase] ordinal of [generation]. */
    external fun nativePhase(generation: Long): Int

    // --- ordinals mirrored from src/core/host_runtime (keep in sync) ---

    /** Phase ordinals — mirror Core::HostRuntime::Phase. */
    object PhaseOrdinal {
        const val IDLE = 0
        const val PREPARING = 1
        const val READY = 2
        const val RUNNING = 3
        const val STOPPING = 4
        const val STOPPED = 5
        const val FAILED = 6
    }

    /** RunOutcome ordinals — mirror Core::HostRuntime::RunOutcome. */
    object Outcome {
        const val RETURNED = 0
        const val CANCELLED = 1
        const val FAULTED = 2
        const val BACKEND_FAILED = 3
        const val UNSUPPORTED = 4
        const val UNEXPECTED = 5
        const val START_FAILED = 6
        const val TIMEOUT = -1
    }

    /** StopResult ordinals — mirror Core::HostRuntime::StopResult. */
    object Stop {
        const val ACCEPTED = 0
        const val CANCEL_PENDING = 1
        const val ALREADY_STOPPING = 2
        const val ALREADY_STOPPED = 3
        const val WRONG_GENERATION = 4
        const val TIMEOUT = 5
        const val ERROR = 6
    }

    /** WaitPhaseResult ordinals — mirror Core::HostRuntime::WaitPhaseResult. */
    object WaitPhase {
        const val REACHED_TARGET = 0
        const val TERMINATED_BEFORE_TARGET = 1
        const val WRONG_GENERATION = 2
        const val TIMEOUT = 3
    }
}
