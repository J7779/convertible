import javax.inject.Inject

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Repository root: apps/android/app -> ../../..
val repoRoot: File = rootProject.projectDir.resolve("../..").canonicalFile

// The app shares its version with the core (`project(Convertible VERSION x.y.z)` in the root CMakeLists.txt).
val appVersion: String = run {
    val cmake = repoRoot.resolve("CMakeLists.txt")
    val match = if (cmake.isFile) Regex("""project\(\s*Convertible\s+VERSION\s+([0-9.]+)""").find(cmake.readText()) else null
    match?.groupValues?.get(1) ?: "0.1.0"
}
val appVersionCode: Int = appVersion.split(".").map { it.toIntOrNull() ?: 0 }
    .let { it.getOrElse(0) { 0 } * 10000 + it.getOrElse(1) { 0 } * 100 + it.getOrElse(2) { 0 } }
    .coerceAtLeast(1)

// ABIs to build. All three by default; `-Pconvertible.abis=arm64-v8a` speeds up local iteration.
val targetAbis: List<String> = ((findProperty("convertible.abis") as String?) ?: "arm64-v8a,armeabi-v7a,x86_64")
    .split(',').map { it.trim() }.filter { it.isNotEmpty() }

// Parallel compile jobs for the native build (the core is large); keep shared build machines usable.
val nativeJobs: String = (findProperty("convertible.nativeJobs") as String?) ?: "6"

android {
    namespace = "app.convertible"
    compileSdk = 37
    ndkVersion = "27.1.12297006"

    defaultConfig {
        applicationId = "app.convertible"
        minSdk = 26
        targetSdk = 37
        versionCode = appVersionCode
        versionName = appVersion
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters.clear()
            abiFilters += targetAbis
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DCVT_ANDROID_JOBS=$nativeJobs",
                )
                targets += "convertible_jni"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // No signing config: release APKs are produced unsigned and signed by the release pipeline.
        }
        debug {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        jniLibs {
            // Store .so files uncompressed and 16 KB aligned so they load straight from the APK.
            useLegacyPackaging = false
        }
        resources {
            excludes += setOf("META-INF/{AL2.0,LGPL2.1}", "META-INF/*.version", "DebugProbesKt.bin", "kotlin-tooling-metadata.json")
        }
    }

    // One APK per ABI plus a universal one (store uploads would use an App Bundle instead).
    splits {
        abi {
            isEnable = true
            reset()
            include(*targetAbis.toTypedArray())
            isUniversalApk = true
        }
    }

    testOptions {
        unitTests.isReturnDefaultValues = false
    }

    lint {
        abortOnError = true
        checkReleaseBuilds = true
        // Version-bump hints depend on the day the build runs; versions are pinned on purpose.
        disable += setOf("GradleDependency", "NewerVersionAvailable", "AndroidGradlePluginVersion", "OldTargetApi")
    }
}

// ---------------------------------------------------------------------------------------------
// Brand resources: fonts, launcher icon layers, design tokens, the motion storyboard and licence
// texts are taken from the shared assets/ directory at build time, so there is one source of truth.
// ---------------------------------------------------------------------------------------------
abstract class BrandResourcesTask : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val brandDir: DirectoryProperty

    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val fontsDir: DirectoryProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val licenseFile: RegularFileProperty

    @get:OutputDirectory
    abstract val resOutput: DirectoryProperty

    @get:OutputDirectory
    abstract val assetsOutput: DirectoryProperty

    @get:Inject
    abstract val fs: FileSystemOperations

    @TaskAction
    fun generate() {
        val brand = brandDir.get().asFile
        val fonts = fontsDir.get().asFile
        val res = resOutput.get().asFile
        val assets = assetsOutput.get().asFile
        fs.delete { delete(res, assets) }
        val fontNames = mapOf(
            "Inter-Regular.ttf" to "inter_regular.ttf",
            "Inter-Medium.ttf" to "inter_medium.ttf",
            "Inter-SemiBold.ttf" to "inter_semibold.ttf",
            "JetBrainsMonoNL-Regular.ttf" to "jetbrains_mono_regular.ttf",
            "JetBrainsMonoNL-Medium.ttf" to "jetbrains_mono_medium.ttf",
        )
        fontNames.forEach { (from, to) ->
            fs.copy { from(fonts.resolve(from)); into(res.resolve("font")); rename { to } }
        }
        val icons = brand.resolve("icons/android")
        fs.copy {
            from(icons) { include("ic_launcher_background.xml", "ic_launcher_foreground.xml", "ic_launcher_monochrome.xml") }
            into(res.resolve("drawable"))
        }
        listOf("ic_launcher.xml", "ic_launcher_round.xml").forEach { name ->
            fs.copy { from(icons.resolve("ic_launcher.xml")); into(res.resolve("mipmap-anydpi")); rename { name } }
        }
        fs.copy { from(brand) { include("tokens.json", "animation.json") }; into(assets.resolve("brand")) }
        fs.copy { from(licenseFile); into(assets.resolve("licenses")); rename { "Convertible-LICENSE.txt" } }
        fs.copy { from(fonts) { include("*-LICENSE.txt") }; into(assets.resolve("licenses")) }
    }
}

val brandResources = tasks.register<BrandResourcesTask>("generateBrandResources") {
    brandDir.set(repoRoot.resolve("assets/brand"))
    fontsDir.set(repoRoot.resolve("assets/fonts"))
    licenseFile.set(repoRoot.resolve("LICENSE"))
    resOutput.set(layout.buildDirectory.dir("generated/brand/res"))
    assetsOutput.set(layout.buildDirectory.dir("generated/brand/assets"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.res?.addGeneratedSourceDirectory(brandResources, BrandResourcesTask::resOutput)
        variant.sources.assets?.addGeneratedSourceDirectory(brandResources, BrandResourcesTask::assetsOutput)
    }
}

// JVM unit tests read the real tokens.json / animation.json from the repository.
tasks.withType<Test>().configureEach {
    systemProperty("convertible.brandDir", repoRoot.resolve("assets/brand").absolutePath)
}

dependencies {
    val composeBom = platform(libs.compose.bom)
    implementation(composeBom)
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.graphics)
    implementation(libs.compose.ui.text)
    implementation(libs.compose.foundation)
    implementation(libs.compose.animation)
    implementation(libs.compose.ui.tooling.preview)
    implementation(libs.activity.compose)
    implementation(libs.lifecycle.viewmodel.compose)
    implementation(libs.lifecycle.runtime.compose)
    implementation(libs.coroutines.android)
    debugImplementation(libs.compose.ui.tooling)
    debugImplementation(libs.compose.ui.test.manifest)

    testImplementation(libs.junit)
    testImplementation(libs.coroutines.test)

    androidTestImplementation(composeBom)
    androidTestImplementation(libs.androidx.test.core)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.test.rules)
    androidTestImplementation(libs.androidx.test.ext.junit)
    androidTestImplementation(libs.androidx.test.espresso.core)
    androidTestImplementation(libs.compose.ui.test.junit4)
    androidTestImplementation(libs.coroutines.test)
}
