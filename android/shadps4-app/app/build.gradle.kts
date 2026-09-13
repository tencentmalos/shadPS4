import java.util.Properties
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.compose.compiler)
    alias(libs.plugins.hilt)
    alias(libs.plugins.ksp)
}

android {
    namespace = "com.shadps4.android"
    compileSdk = 36

    val localProperties = Properties()
    val localPropertiesFile = rootProject.file("local.properties")
    if (localPropertiesFile.exists()) {
        localPropertiesFile.inputStream().use {
            localProperties.load(it)
        }
    }

    val versionProperties = Properties()
    val versionPropertiesFile = rootProject.file("version.properties")
    if (versionPropertiesFile.exists()) {
        versionPropertiesFile.inputStream().use {
            versionProperties.load(it)
        }
    }

    val releaseKeystoreName = localProperties.getProperty("signing.storeFile")
    val releaseKeystoreFile =
        if (releaseKeystoreName != null) rootProject.file(releaseKeystoreName) else null
    val hasReleaseKeystore = releaseKeystoreFile != null && releaseKeystoreFile.exists()
    if (hasReleaseKeystore) {
        signingConfigs {
            create("release") {
                storeFile = releaseKeystoreFile
                storePassword = localProperties.getProperty("signing.storePassword")
                keyAlias = localProperties.getProperty("signing.keyAlias")
                keyPassword = localProperties.getProperty("signing.keyPassword")
            }
        }
    }

    defaultConfig {
        applicationId = "com.shadps4.android"
        minSdk = 33
        targetSdk = 35
        // Priority: CLI -P only > version.properties > date-based dev defaults
        // (do not read VERSION_* from gradle.properties — that fights AutoUpdate)
        val cliVersionCode = gradle.startParameter.projectProperties["VERSION_CODE"]
        val cliVersionName = gradle.startParameter.projectProperties["VERSION_NAME"]
        versionCode = cliVersionCode?.toIntOrNull()
            ?: versionProperties.getProperty("VERSION_CODE")?.toIntOrNull()
            ?: SimpleDateFormat("yyMMddHH").format(Date()).toInt()
        versionName = cliVersionName
            ?: versionProperties.getProperty("VERSION_NAME")
            ?: ("0.1.0-dev-" + SimpleDateFormat("yyyyMMdd-HHmm").format(Date()))
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        // Build-time metadata for diagnostic reports (never run git on device).
        val sourceCommit = try {
            val p = ProcessBuilder("git", "rev-parse", "--short=12", "HEAD")
                .directory(rootProject.projectDir.resolve("../.."))
                .redirectError(ProcessBuilder.Redirect.DISCARD)
                .start()
            val out = p.inputStream.bufferedReader().readText().trim()
            if (p.waitFor() == 0 && out.isNotBlank()) out else "unknown"
        } catch (_: Exception) {
            "unknown"
        }
        val runtimeRevision = try {
            val lock = rootProject.projectDir.resolve("../../runtime/locks/components.lock.json")
            if (!lock.isFile) {
                "unknown"
            } else {
                val body = lock.readText()
                Regex("\"revision\"\\s*:\\s*\"([^\"]+)\"").find(body)?.groupValues?.get(1)
                    ?: Regex("\"sha256\"\\s*:\\s*\"([0-9a-fA-F]{12,})\"").find(body)?.groupValues?.get(1)?.take(16)
                    ?: "unknown"
            }
        } catch (_: Exception) {
            "unknown"
        }
        buildConfigField("String", "SOURCE_COMMIT", "\"${sourceCommit.replace("\"", "").replace("\n", "")}\"")
        buildConfigField("String", "RUNTIME_REVISION", "\"${runtimeRevision.replace("\"", "").replace("\n", "").take(64)}\"")
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            // Monorepo subdir builds do not embed VCS consistently; omit so local
            // and F-Droid APKs both lack version-control-info rather than diverge.
            vcsInfo.include = false
            // Only attach signing when a local release keystore is configured.
            // F-Droid builds strip signing config and must remain unsigned here.
            signingConfigs.findByName("release")?.let { signingConfig = it }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        buildConfig = true
        compose = true
    }

    flavorDimensions += "distribution"
    productFlavors {
        create("playstore") {
            dimension = "distribution"
            buildConfigField("Boolean", "DOWNLOAD_RUNTIME", "false")
            // Play: bundled Turnip only — skip driver picker in setup/settings.
            buildConfigField("Boolean", "SHOW_DRIVER_SELECTION", "false")
        }
        create("fdroid") {
            dimension = "distribution"
            buildConfigField("Boolean", "DOWNLOAD_RUNTIME", "false")
            buildConfigField("Boolean", "SHOW_DRIVER_SELECTION", "true")
        }
    }
    androidResources {
        noCompress += listOf("zip", "json")
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
            // Keep unstripped .so files so F-Droid vs developer strip steps cannot diverge.
            keepDebugSymbols += "**/*.so"
        }
    }
}

// F-Droid reproducible builds: baseline.prof / baseline.profm are often non-deterministic.
// https://f-droid.org/docs/Reproducible_Builds/#bug-baselineprof-not-deterministic
tasks.whenTaskAdded {
    if (name.contains("ArtProfile")) {
        enabled = false
    }
}

// The reference project's post-assemble verifyNativeRuntimeFixes gate (which checked the APK for
// Vortek/Winlator native markers via runtime/tests/verify-native-fixes.mjs) is intentionally
// removed: this replica discards the winlator/vortek native model and ships the in-process FEX
// session library instead, so those markers do not — and must not — exist.

androidComponents {
    beforeVariants { variantBuilder ->
        val startParameterTasks = gradle.startParameter.taskNames
        val hasPlaystoreExplicitly = startParameterTasks.any { it.contains("playstore", ignoreCase = true) }
        val hasGenericAssemble = startParameterTasks.any { 
            it.endsWith("assemble") || 
            it.endsWith("assembleDebug") || 
            it.endsWith("assembleRelease") || 
            it.endsWith("build") 
        }
        
        if (variantBuilder.flavorName == "playstore" && hasGenericAssemble && !hasPlaystoreExplicitly) {
            variantBuilder.enable = false
        }
    }
}

dependencies {
    implementation(project(":core:designsystem"))
    implementation(project(":core:data"))
    implementation(project(":core:runtime"))
    implementation(project(":feature:setup"))
    implementation(project(":feature:library"))
    implementation(project(":feature:session"))
    implementation(project(":feature:settings"))
    implementation(project(":feature:drivers"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.hilt.navigation.compose)
    implementation("androidx.documentfile:documentfile:1.1.0")
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.material3)
    implementation(libs.hilt.android)
    ksp(libs.hilt.compiler)
    ksp(libs.kotlin.metadata.jvm)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(platform(libs.compose.bom))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
}

// Rebuild our own ELF test content from source; never package games or checked-in
// binary fixtures. These assets belong only to the instrumentation APK.
val runtimeFixtureAssets = layout.buildDirectory.dir("generated/productionRuntimeAssets")
android.sourceSets.getByName("androidTest").assets.srcDir(runtimeFixtureAssets)
val fixtureRepo = rootProject.projectDir.resolve("../..")
val fixtureNdk = android.sdkDirectory.resolve("ndk/29.0.14206865")
val runtimeFixtureTasks = listOf("gpu-flip", "storage", "storage-read", "save-dialog", "videoout", "videoout-bad", "videoout-format", "bootstrap", "bootstrap-wait", "libc", "libc-wait", "services", "fixture", "self", "wait", "dependency", "dependency-wait", "bad", "unknown").map { kind ->
    tasks.register<Exec>("generate${kind.replaceFirstChar { it.uppercase() }}RuntimeElf") {
        inputs.file(fixtureRepo.resolve("scripts/android/generate-production-runtime-fixture"))
        inputs.file(fixtureRepo.resolve("tests/guest_cpu/fixtures/production_runtime.S"))
        inputs.file(fixtureRepo.resolve("tests/guest_cpu/fixtures/runtime_services.S"))
        inputs.file(fixtureRepo.resolve("tests/guest_cpu/fixtures/runtime_videoout.S"))
        inputs.file(fixtureRepo.resolve("tests/guest_cpu/fixtures/runtime_gpu_flip.S"))
        inputs.file(fixtureRepo.resolve("tests/guest_cpu/fixtures/runtime_storage.S"))
        inputs.file(fixtureRepo.resolve("tests/guest_cpu/fixtures/runtime_save_dialog.S"))
        val output = runtimeFixtureAssets.get().file(if (kind == "dependency") "fixture_dependency.sprx" else "$kind.elf").asFile
        outputs.file(output)
        if (kind == "storage") outputs.file(runtimeFixtureAssets.get().file("sce_sys/param.sfo"))
        commandLine(listOf("python3", fixtureRepo.resolve("scripts/android/generate-production-runtime-fixture").absolutePath,
            "--ndk", fixtureNdk.absolutePath, "--out", output.absolutePath) +
            when (kind) {
                "bootstrap", "bootstrap-wait" -> listOf("--with-dependency", "--libc")
                "libc" -> listOf("--module", "--libc")
                "libc-wait" -> listOf("--module", "--libc", "--wait")
                "services" -> listOf("--services")
                "storage" -> listOf("--storage")
                "storage-read" -> listOf("--storage", "--read-save")
                "save-dialog" -> listOf("--save-dialog")
                "gpu-flip" -> listOf("--gpu-flip")
                "videoout" -> listOf("--videoout")
                "videoout-bad" -> listOf("--videoout", "--bad-pointer")
                "videoout-format" -> listOf("--videoout", "--bad-format")
                "self" -> listOf("--self", "--with-dependency")
                "wait" -> listOf("--wait")
                "dependency" -> listOf("--module")
                "dependency-wait" -> listOf("--module", "--wait")
                "bad" -> listOf("--bad-pointer", "--with-dependency")
                "unknown" -> listOf("--unknown-import", "--with-dependency")
                else -> listOf("--with-dependency")
            })
    }
}
tasks.configureEach {
    if (name.startsWith("merge") && name.endsWith("AndroidTestAssets")) dependsOn(runtimeFixtureTasks)
}

// Native host profile: checksum-pinned bionic Turnip, generated outside source.
val nativeTurnipAssets = layout.buildDirectory.dir("generated/nativeTurnipAssets")
android.sourceSets.getByName("main").assets.srcDir(nativeTurnipAssets)
val prepareNativeTurnip = tasks.register<Exec>("prepareNativeTurnip") {
    inputs.file(fixtureRepo.resolve("runtime/locks/turnip-bionic.json"))
    inputs.file(fixtureRepo.resolve("scripts/android/prepare-bionic-turnip"))
    outputs.dir(nativeTurnipAssets)
    commandLine("python3", fixtureRepo.resolve("scripts/android/prepare-bionic-turnip").absolutePath,
        "--out", nativeTurnipAssets.get().dir("native-turnip").asFile.absolutePath)
}
tasks.named("preBuild").configure { dependsOn(prepareNativeTurnip) }
