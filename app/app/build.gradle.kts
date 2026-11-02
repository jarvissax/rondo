import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.serialization")
}

// Release signing (docs/building.md): RONDO_SIGNING names a keystore.properties kept outside the
// repo, with storeFile (relative to that file), storePassword, keyAlias and keyPassword. Without
// it, release builds come out unsigned.
val signing: Pair<Properties, File>? = System.getenv("RONDO_SIGNING")?.takeIf { it.isNotEmpty() }?.let { path ->
    val f = file(path)
    Properties().apply { f.inputStream().use { load(it) } } to f.absoluteFile.parentFile
}

android {
    namespace = "com.jarvissax.rondo"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.jarvissax.rondo"
        minSdk = 17
        targetSdk = 22
        versionCode = 100
        versionName = "1.0.0"
        multiDexEnabled = true
    }

    signingConfigs {
        if (signing != null) create("release") {
            val (p, dir) = signing
            storeFile = File(dir, p.getProperty("storeFile"))
            storePassword = p.getProperty("storePassword")
            keyAlias = p.getProperty("keyAlias")
            keyPassword = p.getProperty("keyPassword")
        }
    }

    buildTypes {
        release {
            if (signing != null) signingConfig = signingConfigs.getByName("release")
        }
    }

    lint {
        // Google Play's minimum target doesn't apply: Rondo is installed by hand on Android 4.2
        disable += "ExpiredTargetSdkVersion"
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }

    kotlinOptions {
        jvmTarget = "1.8"
    }
}

dependencies {
    implementation("androidx.multidex:multidex:2.0.1")
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("androidx.core:core-ktx:1.10.1")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.5.1") // the version appcompat 1.6.1 already uses

    // ExoPlayer 2.19.1: the last ExoPlayer 2 release, minSdk 16 (Media3 is not an option)
    implementation("com.google.android.exoplayer:exoplayer-core:2.19.1")
    implementation("com.google.android.exoplayer:exoplayer-dash:2.19.1")
    implementation("com.google.android.exoplayer:exoplayer-ui:2.19.1")
    implementation("com.google.android.exoplayer:extension-mediasession:2.19.1")
    implementation("androidx.media:media:1.6.0") // 1.7.0 declares minSdk 19

    // OkHttp 3.12.x is the last line that runs below API 21
    implementation("com.squareup.okhttp3:okhttp:3.12.13")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.6.3")

    // the dashboard (docs/dev/dashboard-api.md): a tiny HTTP server and the QR code of its address; both
    // plain Java, fine on API 17
    implementation("org.nanohttpd:nanohttpd:2.3.1")
    implementation("com.google.zxing:core:3.3.3")
}
