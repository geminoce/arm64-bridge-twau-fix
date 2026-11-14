# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$SdkRoot = $env:ANDROID_HOME,
    [string]$Python = 'python',
    [string]$BuildDirectory,
    [switch]$JniContractChecks,
    [switch]$FmodDiagnostic,
    [switch]$RenderDocGlesLayer,
    [string]$RenderDocLayerPath,
    [int]$Parallel = 6
)
$ErrorActionPreference = 'Stop'
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_SDK_ROOT }
if (-not $SdkRoot) { $SdkRoot = Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
$Python = (Get-Command -Name $Python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$BridgeRoot = $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $BridgeRoot 'build_arm64' }
$Ndk = Join-Path $SdkRoot 'ndk/27.2.12479018'
$CmakeRoot = Join-Path $SdkRoot 'cmake/3.22.1'
$Cmake = Join-Path $CmakeRoot 'bin/cmake.exe'
$Ninja = Join-Path $CmakeRoot 'bin/ninja.exe'
$Toolchain = Join-Path $Ndk 'build/cmake/android.toolchain.cmake'
$ContractOption = if ($JniContractChecks) { 'ON' } else { 'OFF' }
$FmodOption = if ($FmodDiagnostic) { 'ON' } else { 'OFF' }
$RenderDocOption = if ($RenderDocGlesLayer) { 'ON' } else { 'OFF' }
if (-not $RenderDocLayerPath) { $RenderDocLayerPath = Join-Path $BridgeRoot 'third_party/renderdoc/libVkLayer_GLES_RenderDoc.so' }
foreach ($Required in @($Cmake, $Ninja, $Toolchain, $Python)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) { throw "Missing build tool: $Required" }
}
if ($RenderDocGlesLayer -and -not (Test-Path -LiteralPath $RenderDocLayerPath -PathType Leaf)) {
    throw "Missing ARM64 RenderDoc GLES layer: $RenderDocLayerPath"
}
& $Cmake -S $BridgeRoot -B $BuildDirectory -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=android-26' '-DANDROID_STL=c++_static' '-DCMAKE_BUILD_TYPE=Release' "-DPython3_EXECUTABLE=$Python" "-DWOLF_JNI_CONTRACT_CHECKS=$ContractOption" "-DWOLF_FMOD_DIAGNOSTIC=$FmodOption" "-DWOLF_RENDERDOC_GLES_LAYER=$RenderDocOption" "-DWOLF_RENDERDOC_GLES_LAYER_LIBRARY=$RenderDocLayerPath" '-DFETCHCONTENT_FULLY_DISCONNECTED=ON' '-DFETCHCONTENT_UPDATES_DISCONNECTED=ON'
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
& $Cmake --build $BuildDirectory --target main --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw "ARM64 build failed: $LASTEXITCODE" }
$Library = Join-Path $BuildDirectory 'libmain.so'
$PackagingLibrary = Join-Path $BuildDirectory 'libmain.packaged.so'
$Strip = Join-Path $Ndk 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe'
& $Strip --strip-unneeded -o $PackagingLibrary $Library
if ($LASTEXITCODE -ne 0) { throw 'Host packaging strip failed' }
$Readelf = Join-Path $Ndk 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe'
$Header = (& $Readelf -h $PackagingLibrary) -join "`n"
if ($Header -notmatch 'Class:\s+ELF64' -or $Header -notmatch 'Machine:\s+AArch64') { throw 'Expected ELF64/AArch64 bridge' }
$Loads = & $Readelf -lW $PackagingLibrary | Select-String '^\s*LOAD\s'
if (-not $Loads) { throw 'Missing PT_LOAD segments' }
foreach ($Load in $Loads) {
    if ($Load.Line -notmatch '\s(0x[0-9a-fA-F]+)\s*$' -or [Convert]::ToUInt64($Matches[1].Substring(2),16) -lt 0x4000) { throw "Invalid 16KiB alignment: $($Load.Line)" }
}
Get-FileHash -Algorithm SHA256 -LiteralPath $Library,$PackagingLibrary
