import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins { id("com.android.library"); id("org.jetbrains.kotlin.android") }
android {
    namespace = "dev.openlens.encoder"
    compileSdk = 36
    defaultConfig { minSdk = 31 }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
}
kotlin { compilerOptions { jvmTarget.set(JvmTarget.JVM_17); allWarningsAsErrors.set(true) } }
