# On My Disk Mobile (Capacitor)

Native Android and iOS applications for On My Disk using [Capacitor](https://capacitorjs.com/).

## Project Structure

- `android/`: Native Android Studio project.
- `ios/`: Native iOS Xcode project.
- `capacitor.config.ts`: Capacitor configuration pointing to `../svar/dist`.
- `package.json`: Scripts for syncing and building.

## Development Workflow

1. **Build the Svelte Frontend**:
   ```bash
   cd ../svar
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

## Over-The-Air (OTA) Live Updates

The mobile app includes `@capgo/capacitor-updater` with self-hosted updates from `https://forge.onmydisk.net/repo/Android/`:
- `omd-frontend.zip` (compressed static web bundle)
- `version.json` (`{"version":"1.0.<BUILD_NUMBER>"}`)
- Synchronous `notifyAppReady()` on startup prevents rollbacks.
