# Building the Android app with VS Code (no Android Studio) — Fedora/Linux

Yes, VS Code is fine. You don't need Android Studio. You need 4 things:
JDK 17 + Android SDK (cmdline tools) + Gradle + (NDK/CMake come via SDK).

## 1. Install JDK 17 + extensions (5 min)

```bash
sudo dnf install -y java-17-openjdk-devel
java -version   # must say 17
```

VS Code extensions (install from Extensions tab):
- `Android for VS Code` (or `Gradle for Java` + `Kotlin` language support)
- `Gradle for Java` (Microsoft) — gives you Tasks: Build/Assemble

## 2. Install Android SDK command-line tools (~10 min, one time)

```bash
mkdir -p ~/Android/Sdk/cmdline-tools
cd ~/Android/Sdk/cmdline-tools
# download from: https://developer.android.com/studio#command-line-tools-only
# file: commandlinetools-linux-11076708_latest.zip
unzip ~/Downloads/commandlinetools-linux-*_latest.zip
mv cmdline-tools latest
export ANDROID_HOME=~/Android/Sdk
export ANDROID_SDK_ROOT=~/Android/Sdk
export PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH

# accept licences + install what our app/build.gradle needs:
yes | sdkmanager --licenses
sdkmanager "platform-tools" "platforms;android-34" "build-tools;34.0.0" \
  "ndk;26.1.10909125" "cmake;3.22.1"
```

Put these 4 lines in your `~/.bashrc` so every terminal has them:
```bash
export ANDROID_HOME=~/Android/Sdk
export ANDROID_SDK_ROOT=~/Android/Sdk
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk
export PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH
```

## 3. Tell Gradle where the SDK is

Create `android/local.properties` (never committed — it's gitignored):

```properties
sdk.dir=/home/nitin/Android/Sdk
```

## 4. Build from VS Code terminal

```bash
cd /home/nitin/Documents/programming/cppfile/android
gradle wrapper --gradle-version 8.7   # one time (or use system gradle)
./gradlew assembleDebug
# APK lands at: app/build/outputs/apk/debug/app-debug.apk
```

Install to a phone over USB (enable USB debugging on the phone):

```bash
adb devices                      # phone must show as "device"
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## 5. Run (same as before)

1. Both phones: same WiFi hotspot. One bud connected per phone.
2. Master phone: open SyncWifi → START AS MASTER → accept capture
   consent → play YouTube/Chrome (NOT Netflix — DRM = silence).
3. Friend phone: SyncWifi → START AS CLIENT.
4. Watch client status: `level≈1920 ratio≈1.000000 lost=0`.

## Troubleshooting

- `SDK location not found` → you skipped step 3 (`local.properties`).
- `NDK not found` → re-run the `sdkmanager "ndk;..." "cmake;..."` line.
- `JAVA_HOME` errors → `sudo dnf install java-17-openjdk-devel`, log out/in.
- First build downloads ~500MB of Gradle deps. Normal. Go make chai.
- Phone not in `adb devices` → phone: enable Developer Options
  (tap Build Number 7x) → USB debugging ON → replug → accept prompt.
