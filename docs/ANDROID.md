# Building the Android app (no Android Studio required)

You need 4 things: **JDK 17 + Android SDK (cmdline tools) + Gradle**. The NDK
and CMake come via the SDK. These instructions are written for Fedora/Linux
with VS Code; substitute your own package manager elsewhere.

## 1. Install JDK 17

```bash
sudo dnf install -y java-17-openjdk-devel   # apt: openjdk-17-jdk
java -version   # must say 17
```

VS Code extensions: `Gradle for Java` (Microsoft) and `Kotlin` language
support.

## 2. Install the Android SDK command-line tools (one time)

```bash
mkdir -p ~/Android/Sdk/cmdline-tools && cd ~/Android/Sdk/cmdline-tools
# download "commandlinetools-linux-*_latest.zip" from
# https://developer.android.com/studio#command-line-tools-only
unzip ~/Downloads/commandlinetools-linux-*_latest.zip
mv cmdline-tools latest

export ANDROID_HOME=~/Android/Sdk
export ANDROID_SDK_ROOT=~/Android/Sdk
export PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH

yes | sdkmanager --licenses
sdkmanager "platform-tools" "platforms;android-34" "build-tools;34.0.0" \
  "ndk;26.1.10909125" "cmake;3.22.1"
```

Add these to `~/.bashrc` so every terminal has them:

```bash
export ANDROID_HOME=~/Android/Sdk
export ANDROID_SDK_ROOT=~/Android/Sdk
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk
export PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH
```

## 3. Tell Gradle where the SDK is

Create `android/local.properties` (gitignored — never committed):

```properties
sdk.dir=/home/YOUR_USER/Android/Sdk
```

## 4. Build

```bash
cd android
./gradlew assembleDebug
# → app/build/outputs/apk/debug/app-debug.apk
```

Install to a phone over USB (enable Developer Options → USB debugging):

```bash
adb devices                                        # must show "device"
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## 5. Run

1. Install the **same APK on both phones**.
2. **Master**: open OMEH → *START AS MASTER* → accept the screen-capture
   consent → a QR code appears.
3. **Client**: open OMEH → *SCAN TO JOIN* → scan the master's QR → accept
   the Wi-Fi Direct invitation.
4. On the master, start playback (YouTube, Chrome, music app — **not Netflix**,
   which is DRM-protected and captures as silence).
5. Watch the client status line: `level≈1920 ratio≈1.000000 lost=0`.

Both phones need **Wi-Fi and Location services ON** for peer discovery.

## Source layout

```
android/app/src/main/
├── AndroidManifest.xml          permissions + component names
├── cpp/
│   ├── CMakeLists.txt           includes ../../../../../protocol/sync_wifi.h
│   └── native-lib.cpp           JNI bridge to the shared sync engine
└── java/com/syncwifi/
    ├── SyncWifiApp.kt           Application; installs CrashHandler
    ├── CrashHandler.kt          uncaught-exception → file + CrashActivity
    ├── ui/       MainActivity.kt, CrashActivity.kt
    ├── p2p/      WifiP2pManagerWrapper.kt
    ├── stream/   MasterService.kt, ClientService.kt, NativeBridge.kt
    └── util/     AppLog.kt
```

## Troubleshooting

- `SDK location not found` → you skipped step 3 (`local.properties`).
- `NDK not found` → re-run the `sdkmanager "ndk;..." "cmake;..."` line.
- `JAVA_HOME` errors → install JDK 17, then log out and back in.
- First build downloads ~500 MB of Gradle dependencies. Normal.
- Phone missing from `adb devices` → enable Developer Options (tap Build Number
  7×) → USB debugging ON → replug → accept the prompt.
- Client never finds the master → see
  [TROUBLESHOOTING.md](TROUBLESHOOTING.md#p2p-pairing-fails).

