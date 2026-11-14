import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Clé de signature des APK release, jamais dans le dépôt. Cherchée dans l'ordre :
//  1. variables d'environnement (GitHub Actions) : PHONECAM_KEYSTORE, PHONECAM_KEYSTORE_PASSWORD,
//     PHONECAM_KEY_ALIAS, PHONECAM_KEY_PASSWORD ;
//  2. android/keystore.properties ;
//  3. ~/.phonecam-signing/keystore.properties.
// Sans clé, l'APK release est signé avec la clé de debug (installable, mais pas pour une publication).
val keystoreProps = Properties().apply {
    val candidates = listOf(
        rootProject.file("keystore.properties"),
        File(System.getProperty("user.home"), ".phonecam-signing/keystore.properties"),
    )
    candidates.firstOrNull { it.exists() }?.inputStream()?.use { load(it) }
}

fun signingValue(env: String, key: String): String? =
    System.getenv(env)?.takeIf { it.isNotBlank() } ?: keystoreProps.getProperty(key)?.takeIf { it.isNotBlank() }

val releaseStoreFile = signingValue("PHONECAM_KEYSTORE", "storeFile")
val hasReleaseKey = releaseStoreFile != null && file(releaseStoreFile).exists()

android {
    namespace = "fr.justpita.phonecam"
    compileSdk = 35

    defaultConfig {
        applicationId = "fr.justpita.phonecam"
        minSdk = 29
        targetSdk = 35
        versionCode = 3
        versionName = "2.0.1"
    }

    signingConfigs {
        if (hasReleaseKey) {
            create("release") {
                storeFile = file(releaseStoreFile!!)
                storePassword = signingValue("PHONECAM_KEYSTORE_PASSWORD", "storePassword")
                keyAlias = signingValue("PHONECAM_KEY_ALIAS", "keyAlias") ?: "phonecam"
                keyPassword = signingValue("PHONECAM_KEY_PASSWORD", "keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (hasReleaseKey) signingConfigs.getByName("release") else signingConfigs.getByName("debug")
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

if (!hasReleaseKey) {
    logger.warn("PhoneCam : pas de clé de signature trouvée, l'APK release sera signé avec la clé de debug.")
}
