import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val releaseProperties = Properties().apply {
    rootProject.file("version.properties").inputStream().use { load(it) }
}
val releaseKeystore = providers.gradleProperty("stratocam.keystore")
    .orElse(providers.environmentVariable("STRATOCAM_KEYSTORE")).orNull
val releaseStorePassword = providers.gradleProperty("stratocam.storePassword")
    .orElse(providers.environmentVariable("STRATOCAM_KEYSTORE_PASSWORD")).orNull
val releaseKeyAlias = providers.gradleProperty("stratocam.keyAlias")
    .orElse(providers.environmentVariable("STRATOCAM_KEY_ALIAS")).orNull
val releaseKeyPassword = providers.gradleProperty("stratocam.keyPassword")
    .orElse(providers.environmentVariable("STRATOCAM_KEY_PASSWORD")).orNull

android {
    namespace = "com.androidcam.inspector"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.androidcam.inspector"
        minSdk = 23
        targetSdk = 35
        versionCode = releaseProperties.getProperty("versionCode").toInt()
        versionName = releaseProperties.getProperty("versionName")

        testInstrumentationRunner = "android.test.InstrumentationTestRunner"
    }

    signingConfigs {
        if (listOf(releaseKeystore, releaseStorePassword, releaseKeyAlias, releaseKeyPassword).all { !it.isNullOrBlank() }) {
            create("release") {
                storeFile = file(releaseKeystore!!)
                storePassword = releaseStorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
