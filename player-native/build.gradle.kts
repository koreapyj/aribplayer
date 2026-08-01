plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

val supportedAbis = setOf("arm64-v8a", "x86_64", "armeabi-v7a")
val targetAbi = providers.gradleProperty("targetAbi").getOrElse("arm64-v8a")
require(targetAbi in supportedAbis) {
    "Unsupported targetAbi '$targetAbi'; expected one of ${supportedAbis.joinToString()}"
}

android {
    namespace = "kr.dcmys.android.aribplayer.nativeplayer"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        minSdk = 26

        ndk {
            abiFilters += targetAbi
        }

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += "-DANDROID_STL=c++_shared"
                arguments += "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}
