<#
  Builds the portable ZIP of the series-player build from bin\mpc-hc_x64.
  Run after:  build.bat Build x64 MPCHC Release LAVFilters

    powershell -ExecutionPolicy Bypass -File distrib\series-player\package.ps1

  Output: bin\MPC-HC-series-player_<version>_x64_portable.zip
#>
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$bin  = Join-Path $root 'bin\mpc-hc_x64'
$exe  = Join-Path $bin 'mpc-hc64.exe'
if (-not (Test-Path $exe)) { throw "Not built yet: $exe" }

$version = (Get-Item $exe).VersionInfo.ProductVersion -replace ' .*$', ''
$name    = "MPC-HC-series-player_${version}_x64_portable"
$stage   = Join-Path $root "bin\$name"
$zip     = "$stage.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory $stage | Out-Null

# Program, translations, internal LAV Filters (no debug symbols / link artefacts)
Copy-Item $exe, (Join-Path $bin 'mpciconlib.dll') $stage
# Shared viewing helper (built by src\SeriesShare\build-share.ps1)
if (Test-Path (Join-Path $bin 'mpc-share.exe')) { Copy-Item (Join-Path $bin 'mpc-share.exe') $stage } else { Write-Warning 'mpc-share.exe not built: package without shared viewing' }
Copy-Item (Join-Path $bin 'Lang') (Join-Path $stage 'Lang') -Recurse
New-Item -ItemType Directory (Join-Path $stage 'LAVFilters64') | Out-Null
Get-ChildItem (Join-Path $bin 'LAVFilters64') -File | Where-Object { $_.Extension -in '.dll', '.ax', '.manifest' } |
    Copy-Item -Destination (Join-Path $stage 'LAVFilters64')

# DirectX runtime parts MPC-HC ships with (renderer/shaders)
Copy-Item (Join-Path $root 'distrib\x64\D3DCompiler_47.dll'), (Join-Path $root 'distrib\x64\D3DX9_43.dll') $stage

# Portable mode: an ini next to the exe keeps all settings in this folder (no registry)
Set-Content (Join-Path $stage 'mpc-hc64.ini') -Value '' -Encoding Unicode

# Licence + docs
Copy-Item (Join-Path $root 'COPYING.txt'), (Join-Path $root 'docs\Authors.txt') $stage
Copy-Item (Join-Path $PSScriptRoot 'Інструкція.txt'), (Join-Path $PSScriptRoot 'install.ps1'), (Join-Path $PSScriptRoot 'uninstall.ps1') $stage

if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host ("{0}  ({1:N1} MB)`nSHA256 {2}" -f $zip, ((Get-Item $zip).Length / 1MB), $hash)
