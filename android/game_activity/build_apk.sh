#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/../.." && pwd)
sdk=${ANDROID_HOME:?Set ANDROID_HOME to the Android SDK directory}
java_home=${JAVA_HOME:?Set JAVA_HOME to a JDK directory}
export JAVA_HOME="$java_home"
export PATH="$java_home/bin:$PATH"
tools="$sdk/build-tools/35.0.0"
platform="$sdk/platforms/android-35/android.jar"
output="$here/build"
if [[ ! -f "$output/res/mipmap-xxxhdpi/ic_launcher.png" ]]; then python3 "$repo_root/tools/project_icon.py" "$output/res/mipmap-xxxhdpi/ic_launcher.png"; fi
zsh "$repo_root/games/totk/tools/totk_xlate/publish_android.sh" "${DOTNET:-dotnet}"
ANDROID_LOADER_LIBRARY=1 "$repo_root/android/build_module_loader_probe.sh"
mkdir -p "$output/classes" "$output/dex"
"$java_home/bin/javac" -source 8 -target 8 -Xlint:-options -classpath "$platform" -d "$output/classes" "$here/java/org/macnativegames/totk/gameprobe/VideoSettingsActivity.java"
"$tools/d8" --lib "$platform" --min-api 29 --output "$output/dex" "$output"/classes/org/macnativegames/za/gameprobe/*.class
"$tools/aapt" package -f -M "$here/AndroidManifest.xml" -S "$output/res" -A "$here/assets" -I "$platform" -F "$output/unsigned.apk"
cp "$output/dex/classes.dex" "$output/classes.dex"
(cd "$output" && "$tools/aapt" add unsigned.apk classes.dex)
cp "$repo_root/android/build/shader-translator/libtotkshader.so" "$output/lib/arm64-v8a/"
for hook in main_hook hook_impl file_redirect_hook gsl_alloc_hook; do
  cp "$repo_root/android/build/adrenotools/src/hook/lib$hook.so" "$output/lib/arm64-v8a/"
done
(cd "$output" && "$tools/aapt" add unsigned.apk lib/arm64-v8a/libtotkshader.so lib/arm64-v8a/libtotkgame.so lib/arm64-v8a/libmain_hook.so lib/arm64-v8a/libhook_impl.so lib/arm64-v8a/libfile_redirect_hook.so lib/arm64-v8a/libgsl_alloc_hook.so)
"$tools/zipalign" -f 4 "$output/unsigned.apk" "$output/aligned.apk"
keystore="$output/debug.keystore"
if [[ ! -e "$keystore" ]]; then
  "$java_home/bin/keytool" -genkeypair -keystore "$keystore" -storepass android -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=Android Debug,O=Android,C=US' >/dev/null 2>&1
fi
"$tools/apksigner" sign --ks "$keystore" --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android --out "$output/ZA-Game-Loader.apk" "$output/aligned.apk"
"$tools/apksigner" verify "$output/ZA-Game-Loader.apk"
echo "$output/ZA-Game-Loader.apk"
