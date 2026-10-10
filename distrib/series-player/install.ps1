<#
  Installs the portable series-player build for the current user (no admin rights needed):
    * copies the program to %LOCALAPPDATA%\Programs\MPC-HC-series-player
    * Start menu + desktop shortcuts
    * registers the player for video files ("Open with", Settings > Default apps)
    * folder context menu "Відтворити серії в MPC-HC"

  Run from the unpacked package folder:
    powershell -ExecutionPolicy Bypass -File install.ps1
  Remove again with uninstall.ps1 (copied into the install folder).
#>
$ErrorActionPreference = 'Stop'
$src     = $PSScriptRoot
$dest    = Join-Path $env:LOCALAPPDATA 'Programs\MPC-HC-series-player'
$exe     = Join-Path $dest 'mpc-hc64.exe'
$appName = 'MPC-HC series-player'
$progId  = 'MPCHCSeries.Video'
$exts    = '.mkv', '.mp4', '.m4v', '.avi', '.mov', '.webm', '.wmv', '.ts', '.m2ts', '.mts', '.mpg', '.mpeg',
           '.flv', '.3gp', '.ogv', '.vob', '.divx', '.f4v', '.asf', '.rmvb', '.rm'

if (-not (Test-Path (Join-Path $src 'mpc-hc64.exe'))) { throw "mpc-hc64.exe not found next to install.ps1" }
Get-Process mpc-hc64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dest*" } | ForEach-Object {
    [void]$_.CloseMainWindow(); [void]$_.WaitForExit(8000)
}

# 1. Program files (an existing mpc-hc64.ini with the user's settings and history is kept)
New-Item -ItemType Directory -Force $dest | Out-Null
$keepIni = Test-Path (Join-Path $dest 'mpc-hc64.ini')
Get-ChildItem $src | Where-Object { -not ($keepIni -and $_.Name -eq 'mpc-hc64.ini') } |
    Copy-Item -Destination $dest -Recurse -Force

# 2. Shortcuts
$shell = New-Object -ComObject WScript.Shell
foreach ($dir in [Environment]::GetFolderPath('Programs'), [Environment]::GetFolderPath('Desktop')) {
    $lnk = $shell.CreateShortcut((Join-Path $dir "$appName.lnk"))
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = $dest
    $lnk.IconLocation = "$exe,0"
    $lnk.Description = 'Відеоплеєр для фільмів і серіалів'
    $lnk.Save()
}

# 3. File type registration (HKCU only)
$cmd = "`"$exe`" `"%1`""
function Set-Key([string]$path, [hashtable]$values) {
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
    foreach ($k in $values.Keys) {
        if ($k -eq '(default)') { Set-Item -Path $path -Value $values[$k] }
        else { New-ItemProperty -Path $path -Name $k -Value $values[$k] -PropertyType String -Force | Out-Null }
    }
}
$cls = 'HKCU:\Software\Classes'
Set-Key "$cls\$progId" @{ '(default)' = 'Відеофайл'; 'FriendlyTypeName' = 'Відеофайл' }
Set-Key "$cls\$progId\DefaultIcon" @{ '(default)' = "$exe,0" }
Set-Key "$cls\$progId\shell\open" @{ '(default)' = 'Відтворити в MPC-HC' }
Set-Key "$cls\$progId\shell\open\command" @{ '(default)' = $cmd }

Set-Key "$cls\Applications\mpc-hc64.exe" @{ 'FriendlyAppName' = $appName }
Set-Key "$cls\Applications\mpc-hc64.exe\shell\open\command" @{ '(default)' = $cmd }
$caps = 'HKCU:\Software\MPC-HC-series-player\Capabilities'
Set-Key $caps @{ 'ApplicationName' = $appName; 'ApplicationDescription' = 'Відеоплеєр для фільмів і серіалів'; 'ApplicationIcon' = "$exe,0" }
foreach ($e in $exts) {
    Set-Key "$cls\Applications\mpc-hc64.exe\SupportedTypes" @{ $e = '' }
    Set-Key "$cls\$e\OpenWithProgids" @{ $progId = '' }
    Set-Key "$caps\FileAssociations" @{ $e = $progId }
}
Set-Key 'HKCU:\Software\RegisteredApplications' @{ $appName = 'Software\MPC-HC-series-player\Capabilities' }

# 4. Folder context menu: play all episodes of a folder
Set-Key "$cls\Directory\shell\MPCHCSeries.Play" @{ '(default)' = 'Відтворити серії в MPC-HC'; 'Icon' = "$exe,0" }
Set-Key "$cls\Directory\shell\MPCHCSeries.Play\command" @{ '(default)' = $cmd }

Copy-Item (Join-Path $src 'uninstall.ps1') $dest -Force -ErrorAction SilentlyContinue

# Tell Explorer that associations changed
Add-Type -Namespace Win32 -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);'
[Win32.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)

Write-Host "Встановлено: $dest"
Write-Host "Щоб відео відкривались подвійним клацанням: правий клік на відео > Відкрити за допомогою > Вибрати іншу програму > $appName > Завжди."
