Param(
    [ValidateSet('Debug','Release')]
    [string]$Config = 'Debug',
    [switch]$Rebuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot

if ($Rebuild) {
  cmake -S $repo -B "$repo/build" -A x64 | Out-Host
  cmake --build "$repo/build" --config $Config | Out-Host
}

$dll = Join-Path $repo "build/bin/$Config/AwesomeMenuHost.dll"
if (-not (Test-Path $dll)) { throw "DLL not found: $dll. Use -Rebuild to build it." }

# Per-user COM registration - everything under HKCU\Software\Classes
$guid = '{E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}'
reg add "HKCU\Software\Classes\CLSID\$guid\InprocServer32" /ve /t REG_SZ /d "$dll" /f | Out-Null
reg add "HKCU\Software\Classes\CLSID\$guid\InprocServer32" /v ThreadingModel /t REG_SZ /d Apartment /f | Out-Null

# Context menu handlers under HKCU\Software\Classes (per-user only)
# Using highly selective registration to avoid any overlapping contexts
reg add "HKCU\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\AwesomeMenuHost" /ve /t REG_SZ /d "$guid" /f | Out-Null
reg add "HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost" /ve /t REG_SZ /d "$guid" /f | Out-Null
reg add "HKCU\Software\Classes\*\shellex\ContextMenuHandlers\AwesomeMenuHost" /ve /t REG_SZ /d "$guid" /f | Out-Null

Write-Host "Registered AwesomeMenuHost per-user." -ForegroundColor Green
Write-Host "Restarting Explorer..." -ForegroundColor Yellow
Stop-Process -Name explorer -Force
Start-Process explorer.exe