# Android release signing

StratoCam never stores signing keys or passwords in Git. A public Android release requires a long-lived private key that must be backed up securely; losing it prevents publishing updates under the existing `com.androidcam.inspector` application ID.

Create the ignored local file `.signing/release-signing.properties` with these four entries:

```properties
stratocam.keystore=C:/secure/location/stratocam-release.p12
stratocam.storePassword=replace-with-secret
stratocam.keyAlias=stratocam
stratocam.keyPassword=replace-with-secret
```

The same values can instead be supplied as `STRATOCAM_KEYSTORE`, `STRATOCAM_KEYSTORE_PASSWORD`, `STRATOCAM_KEY_ALIAS`, and `STRATOCAM_KEY_PASSWORD` environment variables. Run `scripts/build-android-release.ps1`; it verifies a signed APK with Android SDK `apksigner` before naming it `dist/StratoCam-Android-v<version>.apk`, using `version.properties`. Without all four values, the script deliberately emits `-unsigned.apk` and does not mislabel it as a publishable release.

Do not use the Android debug keystore for a public release. Do not commit `.signing/`, a keystore, passwords, or CI secrets.
