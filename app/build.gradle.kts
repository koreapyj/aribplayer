import java.util.Properties

val keystorePropertiesFile = rootProject.file("signing/keystore.properties")
val keystoreProperties = Properties().apply {
    if (keystorePropertiesFile.exists()) {
        keystorePropertiesFile.inputStream().use(::load)
    }
}
val hasPinnedSigning = keystorePropertiesFile.exists()

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.ksp)
}

val supportedAbis = setOf("arm64-v8a", "x86_64", "armeabi-v7a")
val targetAbi = providers.gradleProperty("targetAbi").getOrElse("arm64-v8a")
require(targetAbi in supportedAbis) {
    "Unsupported targetAbi '$targetAbi'; expected one of ${supportedAbis.joinToString()}"
}

android {
    namespace = "kr.dcmys.android.aribplayer"
    compileSdk = 35

    defaultConfig {
        applicationId = "kr.dcmys.android.aribplayer"
        minSdk = 26
        targetSdk = 35
        versionCode = 2
        versionName = "0.0.2"

        // Package precisely the native ABI selected by -PtargetAbi.
        ndk {
            abiFilters += targetAbi
        }
    }

    signingConfigs {
        if (hasPinnedSigning) {
            create("pinned") {
                storeFile = rootProject.file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        val buildSigningConfig = if (hasPinnedSigning) {
            signingConfigs.getByName("pinned")
        } else {
            signingConfigs.getByName("debug")
        }

        debug {
            signingConfig = buildSigningConfig
        }

        release {
            signingConfig = buildSigningConfig
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

android.applicationVariants.all {
    outputs.all {
        // AGP's public variant API does not yet expose output filenames.
        (this as com.android.build.gradle.internal.api.BaseVariantOutputImpl).outputFileName =
            "aribplayer-$targetAbi-$name.apk"
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(project(":player-native"))

    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.room.runtime)
    implementation(libs.androidx.room.ktx)
    ksp(libs.androidx.room.compiler)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.androidx.documentfile)

    debugImplementation(libs.androidx.compose.ui.tooling)
}
