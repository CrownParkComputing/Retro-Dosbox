// Release signing.
//
// The keystore comes from ANDROID_KEYSTORE_* environment variables (CI) or a
// key.properties beside this project (a developer machine). No .jks is ever
// committed; both sources are gitignored.
//
// When neither supplies one, release falls back to the debug keystore with a
// warning rather than failing the build -- otherwise an ordinary local build
// of a clean checkout would refuse to run. Such a build is fine to install by
// hand and will not be accepted by Play Console, which is the correct outcome.
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

data class KeystoreConfig(
    val path: String,
    val storePassword: String,
    val keyAlias: String,
    val keyPassword: String,
)

fun resolveKeystore(): KeystoreConfig? {
    val envPath = System.getenv("ANDROID_KEYSTORE_PATH")
    val envStorePw = System.getenv("ANDROID_KEYSTORE_PASSWORD")
    val envAlias = System.getenv("ANDROID_KEY_ALIAS")
    val envKeyPw = System.getenv("ANDROID_KEY_PASSWORD")
    if (envPath != null && envStorePw != null && envAlias != null && envKeyPw != null) {
        logger.lifecycle("release: using keystore from ANDROID_KEYSTORE_PATH")
        return KeystoreConfig(envPath, envStorePw, envAlias, envKeyPw)
    }

    val propsFile = rootProject.file("key.properties")
    if (propsFile.exists()) {
        val p = Properties().apply { load(propsFile.inputStream()) }
        val path = p["storeFile"] as String?
        val storePw = p["storePassword"] as String?
        val alias = p["keyAlias"] as String?
        val keyPw = p["keyPassword"] as String?
        if (path != null && storePw != null && alias != null && keyPw != null) {
            logger.lifecycle("release: using keystore from ${propsFile.absolutePath}")
            return KeystoreConfig(path, storePw, alias, keyPw)
        }
    }

    logger.warn(
        "release: no ANDROID_KEYSTORE_* env vars and no key.properties; " +
            "falling back to the debug keystore. This build will not be " +
            "accepted by Play Console."
    )
    return null
}

val keystoreConfig = resolveKeystore()

// Floor for the version code, carried over from the Flutter app. The Play
// listing has published codes from both the original Java app and the Flutter
// one, and Play never accepts a code at or below what is already there.
val DOSBOX_VERSION_CODE_BASE = 200

android {
    // The code namespace, which is independent of the application id below.
    namespace = "com.crownparkcomputing.retrodos"
    compileSdk = 36
    // Pinned rather than floating: which NDK a build machine happens to have
    // should not decide what ships.
    ndkVersion = "28.2.13676358"

    defaultConfig {
        // Deliberately com.dosboxx.app, NOT the Kotlin package.
        //
        // This is the identity of the existing Play listing, first from a Java
        // app and then from the Flutter one this replaces. Keeping it is what
        // makes this an in-place UPGRADE: existing users get it as an update,
        // reviews and install counts carry over, and the registered signing
        // key keeps working. Publishing under a new id would strand every
        // existing user on the old build with no path forward.
        applicationId = "com.dosboxx.app"
        // 28 is this app's real floor rather than a preference: DOSBox-X needs
        // iconv, and bionic did not gain iconv_open until API 28. Below that
        // the core does not link at all.
        minSdk = 28
        // Play refuses updates to an app targeting more than a year behind the
        // latest release. The app this replaces already targets 36.
        targetSdk = 36
        // 202. The live listing carries 201 on production, alpha and Testers
        // Community, and Play never accepts a code at or below what is there.
        versionCode = DOSBOX_VERSION_CODE_BASE + 2
        versionName = "2.0"
        ndk {
            // Only the ABI the core has actually been built for. Listing more
            // ships an APK that installs and then fails to load a library,
            // which is a worse outcome than not shipping that ABI.
            abiFilters += "arm64-v8a"
        }
    }

    // The core is a prebuilt .so from android/build-core.sh, not something
    // Gradle compiles; it just gets packaged.
    sourceSets["main"].jniLibs.srcDirs("src/main/jniLibs")

    packaging {
        jniLibs {
            // 19 MB of emulator does not benefit from being compressed in the
            // APK and then decompressed at load.
            useLegacyPackaging = true
        }
    }

    signingConfigs {
        if (keystoreConfig != null) {
            create("release") {
                storeFile = file(keystoreConfig.path)
                storePassword = keystoreConfig.storePassword
                keyAlias = keystoreConfig.keyAlias
                keyPassword = keystoreConfig.keyPassword
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (keystoreConfig != null) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
        debug {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    // SDL3's own Java glue, produced by the SDL3 Android build.
    implementation(files("libs/SDL3.jar"))

    // DocumentFile is how a SAF tree is walked; activity gives us the
    // ActivityResultLauncher the folder picker needs.
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.activity:activity-ktx:1.9.3")
}
