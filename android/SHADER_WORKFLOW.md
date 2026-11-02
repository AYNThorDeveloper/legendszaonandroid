# Legends Z-A: prepared cache and local shader translation

Exact memory/disk caches and matching prepared seeds remain first. Missing variants are translated on Android by the bundled `libtotkshader.so`, then validated for the existing reply format and stored under their exact variant key. The Mac bridge is optional. Existing caches, shaders, saves, drivers and environment were not replaced.

The Android NativeAOT library is built on macOS with .NET SDK 10 and the Android NDK. The app itself needs no Python or .NET installation on the Thor. `build_apk.sh` builds and packages the library automatically; set `DOTNET` to the SDK executable if it is not in PATH.

Z-A retains its existing non-constant texture-offset lowering, stable SPIR-V global ordering, 32-lane warp correction and transform-feedback output narrowing/decorations. `AndroidZaFixes.cs` ports the last two operations from the existing Python bridge; `ZA_ANDROID_SHADER_FIXES` enables them for this project. TOTK's gather nudge is not enabled here.

Local host validation: 65 byte-identical replies compared with the working reference, including 20 fragment, 20 vertex, 20 compute requests, one repeated reference and four XFB component-width cases. This is a bounded corpus, not all possible shaders.

Cold-cache gameplay still needs more testing on the AYN Thor. First-use translation may stall. A translation failure remains logged and uses the existing bridge/seed fallback.

`TOTK_SHADER_LOCAL=0` disables the local translator; `TOTK_SHADER_BRIDGE=0` disables only the Mac bridge. The current environment has the bridge disabled and local translation enabled by default.

Source/APK/save backups and installation records: `Test/20261031_local_shaders/`. New translations do not generate optional framebuffer-fetch proof sidecars; without them the existing conservative rendering path remains.
