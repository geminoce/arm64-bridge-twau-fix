# arm64-bridge-twau

ARM32-to-ARM64 compatibility bridge and Android touch controls for The Wolf Among Us.

The bridge runs the game's ARMv7 native libraries inside an ARM64 Android process using Dynarmic. It loads ELF32 images, resolves imports, translates JNI calls and forwards graphics, audio and platform calls to Android.

This is a source repository for the bridge and its controls. It contains no game APKs, proprietary libraries, episode archives, textures, audio, saves or signing keys. You must provide your own legally obtained game files. A standalone game or APK cannot be built from this repository alone.

## Scope

- A32 guest execution on an AArch64 host, with typed ABI adapters and ELF relocation handling.
- JNI, libc, Android, EGL/OpenGL ES and OpenSL ES adapters for the Android release of The Wolf Among Us 1.23.
- Native library alignment for 16 KiB pages and display/frame pacing integration.
- Android touch input with bounded finger slots, multiple pointers and cancellation on lifecycle changes.
- Component tests, bundled source dependencies and the Dynarmic patches used by the bridge.

The adapters include game-specific symbols, layouts and version guards. Support for other game releases or other games requires separate validation.

## Source layout

| Path | Contents |
| --- | --- |
| `arm64_bridge/src/` | Native bridge, loader, JNI and platform adapters |
| `arm64_bridge/tests/` | Native component and ABI contracts |
| `arm64_bridge/third_party/` | Bundled dependency sources with their original licenses |
| `arm64_bridge/patches/dynarmic/` | Patches already applied to the bundled Dynarmic tree |
| `arm64_bridge/build_arm64.ps1` | Windows native build and ELF/alignment checks |
| `arm64_bridge/build_guest_qsort.py` | Builds the bridge's own ARM guest qsort implementation |
| `arm64_bridge/check_symbols.py` | Checks supplied guest ELF imports against bridge providers |
| `controls/src/` | Touch input and display cadence Java adapters |
| `controls/tests/` | Host-side touch slot tests |

## Build the native bridge

The supplied build scripts target Windows and require:

- PowerShell and Python 3.10 or newer, with `python` on `PATH` (or an explicit `-Python` executable).
- Android SDK with **NDK 27.2.12479018** and **CMake 3.22.1**, including Ninja.

Set `ANDROID_HOME` or `ANDROID_SDK_ROOT` to your SDK directory, or pass `-SdkRoot`. The script also checks the standard SDK location under `LOCALAPPDATA`.

From the repository root:

```powershell
& ./arm64_bridge/build_arm64.ps1
```

The target is `arm64-v8a`, Android API 26, C++20, Release, with a static C++ runtime. Dependencies are bundled; CMake is configured to avoid dependency downloads.

Outputs are `arm64_bridge/build_arm64/libmain.so` and the stripped `libmain.packaged.so`. The script checks ELF64/AArch64 headers and at least 16 KiB alignment of every load segment. Build outputs are ignored by Git.

Optional native checks can be compiled with `-JniContractChecks`; `-FmodDiagnostic` enables the bounded FMOD observer. Game-derived movement/scene diagnostic payloads and texture fixtures are excluded from this public source tree.

## Integrate the controls

`controls/src/org/libsdl/app/TouchAdapter.java` and `TouchSlots.java` are intended to be compiled alongside the matching SDL Java activity supplied by the Android game integration. That activity must expose `onNativeTouch(int, int, int, float, float, float)` and the package-visible static state fields `mIsPaused`, `mHasFocus` and `mIsSurfaceReady`.

Route the game surface's touch callback through:

```java
return TouchAdapter.onTouch(view, event);
```

Call `TouchAdapter.releaseAll()` when pausing, losing focus or destroying the surface. Each active Android pointer receives a legacy finger slot; cancellation releases all affected fingers. This adapter emits touch events only.

`controls/src/com/telltalegames/fables100/DisplayCadence.java` matches the bridge's `nativeSetFrameLimit` JNI export. Connect its `attach`, `resume` and `detach` methods to the activity lifecycle. Frame limit profiles are `30`, `45`, `60` and `display` (display cadence), selected with `setFrameLimit`. `handleFrameLimitUrl` handles the corresponding `twau-restoration://fps-limit/` URLs when the surrounding UI provides them.

The original activity, its UI and APK packaging are outside this repository. These Java adapters are integration sources, not a standalone Android application.

## Checks

With JDK 11 or newer on `PATH`, the touch slot self-check runs without Android or game files:

```powershell
javac -d arm64_bridge/build_touch controls/src/org/libsdl/app/TouchSlots.java controls/tests/TouchSlotsTest.java
java -cp arm64_bridge/build_touch org.libsdl.app.TouchSlotsTest
```

After configuring a native build, compile the native component tests using the SDK's CMake:

```powershell
$cmake = Join-Path $env:ANDROID_HOME 'cmake/3.22.1/bin/cmake.exe'
& $cmake --build arm64_bridge/build_arm64 --target wolf_graphics_contract_test wolf_loader_libc_contract_test wolf_fastmem_fpsr_contract_test --parallel 6
```

These executables run on ARM64 Android. The graphics contracts exercise synthetic data and require a working EGL/OpenGL ES driver. Other integration tests, such as display split and real guest import checks, require separately supplied proprietary inputs. Compiling the bridge does not establish compatibility with every Android device or every game scene.

## License

The bridge and control code is copyright 2026 the arm64-bridge-twau contributors and is licensed under **GNU GPL version 3 or, at your option, any later version** (`GPL-3.0-or-later`). See [LICENSE](LICENSE) for the full terms. Distributed modifications and covered combined works must comply with GPL copyleft requirements.

Bundled dependencies retain their own licenses and copyright notices; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This license grants no rights to the original game's proprietary code, content or trademarks. The Wolf Among Us and associated names belong to their respective owners. This is an independent project and is not affiliated with or endorsed by the game's rights holders.
