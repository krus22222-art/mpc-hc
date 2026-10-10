<#
  Builds mpc-share.exe (shared viewing helper) and copies it next to mpc-hc64.exe.

    powershell -ExecutionPolicy Bypass -File src\SeriesShare\build-share.ps1 [-Vcpkg C:\vcpkg] [-BuildDir C:\ssb]

  Requirements: Visual Studio 2022 Build Tools (C++), vcpkg (git clone https://github.com/microsoft/vcpkg).
  Paths are kept short on purpose (long paths break some third-party builds).
#>
param(
    [string]$Vcpkg = "$env:USERPROFILE\vcpkg",
    [string]$BuildDir = "$env:USERPROFILE\ssb"
)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$root = Resolve-Path (Join-Path $src '..\..')
$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
if (-not (Test-Path "$Vcpkg\vcpkg.exe")) { throw "vcpkg not found in $Vcpkg (clone it and run bootstrap-vcpkg.bat)" }

# pkgconf built locally with MSVC: the MSYS2 pkg-config that vcpkg would download is unsigned and is
# blocked when Windows Smart App Control is on.
& "$Vcpkg\vcpkg.exe" install pkgconf:x64-windows
$env:PKG_CONFIG = "$Vcpkg\installed\x64-windows\tools\pkgconf\pkgconf.exe"
$env:VCPKG_KEEP_ENV_VARS = 'PKG_CONFIG'
$env:VCPKG_DISABLE_METRICS = '1'

# libdatachannel with Mbed TLS (OpenSSL's build needs Perl, also blocked by Smart App Control);
# Mbed TLS with DTLS-SRTP on and TLS 1.3 off. See vcpkg-overlay\*\portfile.cmake.
& "$Vcpkg\vcpkg.exe" install --overlay-ports="$src\vcpkg-overlay" `
    'libdatachannel[srtp,ws]:x64-windows-static' opus:x64-windows-static libyuv:x64-windows-static nlohmann-json:x64-windows-static
if ($LASTEXITCODE -ne 0) { throw 'vcpkg install failed' }

$cmake = (Get-ChildItem "$Vcpkg\downloads\tools" -Recurse -Filter cmake.exe | Select-Object -First 1).FullName
$ninja = (Get-ChildItem "$Vcpkg\downloads\tools" -Recurse -Filter ninja.exe | Select-Object -First 1).FullName
$cmd = @"
@echo off
set NoDefaultCurrentDirectoryInExePath=
call "$vs\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo
"$cmake" -S "$src" -B "$BuildDir" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$Vcpkg\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_MANIFEST_MODE=OFF || exit /b 1
"$cmake" --build "$BuildDir" || exit /b 1
"@
$tmp = Join-Path $env:TEMP 'build-mpc-share.cmd'
Set-Content $tmp $cmd -Encoding ascii
cmd /c $tmp
if ($LASTEXITCODE -ne 0) { throw 'mpc-share build failed' }

$dest = Join-Path $root 'bin\mpc-hc_x64'
if (Test-Path $dest) { Copy-Item "$BuildDir\mpc-share.exe" $dest -Force; Write-Host "Copied to $dest" }
Write-Host "Built $BuildDir\mpc-share.exe"
