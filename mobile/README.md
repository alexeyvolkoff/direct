# Disk Direct Mobile (Capacitor)

Native Android and iOS applications for **Disk Direct** using [Capacitor](https://capacitorjs.com/).
Retains package/bundle identifier `net.onmydisk.app` for identity (`DID`) continuity and supports dynamic BYOS gateway switching (`/.well-known/direct-gateway.json`) across `disk.direct`, `direct.onmydisk.net`, and self-hosted enterprise gateways.

## Project Structure

- `android/`: Native Android Studio project (`net.onmydisk.app`, label `Disk Direct`).
- `ios/`: Native iOS Xcode project (`net.onmydisk.app`, display name `Disk Direct`).
- `capacitor.config.ts`: Capacitor configuration pointing to `../web/dist`.
- `package.json`: Scripts for syncing and building (automatically strips `.gz`/`.br` server sidecars from native bundles).

## Development Workflow

1. **Build the Svelte Frontend**:
   ```bash
   cd ../web
   npm run build
   ```

2. **Sync Web Assets to Native Projects**:
   ```bash
   cd ../mobile
   npm run sync
   ```

3. **Open in Native IDEs**:
   ```bash
   npm run open:android
   npm run open:ios
   ```

4. **Build Android APK locally**:
   ```bash
   export JAVA_HOME=/home/alexey/projects/jdk-17
   export PATH=$JAVA_HOME/bin:$PATH
   export ANDROID_HOME=/home/alexey/projects/android-sdk
   cd android && ./gradlew assembleDebug
   ```

## Over-The-Air (OTA) Live Updates (Android Only)

Android builds check `https://forge.disk.direct/repo/DiskDirect/`:
- `Android/direct-frontend.zip` (compressed static web bundle)
- `version.json` (`{"version":"3.0.<BUILD_NUMBER>"}`)
- Synchronous `notifyAppReady()` on startup prevents rollbacks; OTA is disabled on iOS (`isNativePlatform() && getPlatform() === 'android'`).
