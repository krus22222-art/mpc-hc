<#
  Removes the per-user installation made by install.ps1 (shortcuts, file type registration,
  folder context menu, program folder). Settings/history in mpc-hc64.ini are removed too.
    powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\Programs\MPC-HC-series-player\uninstall.ps1"
#>
$dest    = Join-Path $env:LOCALAPPDATA 'Programs\MPC-HC-series-player'
$appName = 'MPC-HC series-player'
$progId  = 'MPCHCSeries.Video'
$cls     = 'HKCU:\Software\Classes'

Get-Process mpc-hc64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dest*" } | Stop-Process -Force

foreach ($dir in [Environment]::GetFolderPath('Programs'), [Environment]::GetFolderPath('Desktop')) {
    Remove-Item -LiteralPath (Join-Path $dir "$appName.lnk") -ErrorAction SilentlyContinue
}
Get-ChildItem "$cls" -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like '.*' } | ForEach-Object {
    $owp = Join-Path $_.PSPath 'OpenWithProgids'
    if (Test-Path $owp) { Remove-ItemProperty -Path $owp -Name $progId -ErrorAction SilentlyContinue }
}
Remove-Item -Path "$cls\$progId" -Recurse -ErrorAction SilentlyContinue
Remove-Item -Path "$cls\Applications\mpc-hc64.exe" -Recurse -ErrorAction SilentlyContinue
Remove-Item -Path "$cls\Directory\shell\MPCHCSeries.Play" -Recurse -ErrorAction SilentlyContinue
Remove-Item -Path 'HKCU:\Software\MPC-HC-series-player' -Recurse -ErrorAction SilentlyContinue
Remove-ItemProperty -Path 'HKCU:\Software\RegisteredApplications' -Name $appName -ErrorAction SilentlyContinue

Set-Location $env:TEMP
Remove-Item -LiteralPath $dest -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'MPC-HC series-player видалено.'
