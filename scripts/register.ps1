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

# Register for specific file types instead of * (which conflicts with folder contexts)
reg add "HKCU\Software\Classes\txtfile\shellex\ContextMenuHandlers\AwesomeMenuHost" /ve /t REG_SZ /d "$guid" /f | Out-Null
reg add "HKCU\Software\Classes\batfile\shellex\ContextMenuHandlers\AwesomeMenuHost" /ve /t REG_SZ /d "$guid" /f | Out-Null
reg add "HKCU\Software\Classes\cmdfile\shellex\ContextMenuHandlers\AwesomeMenuHost" /ve /t REG_SZ /d "$guid" /f | Out-Null

# NOTE: Using specific file types instead of * to prevent folder/file context conflicts
# This should eliminate duplicate menus in navigation pane while maintaining file support

# Note: We only register the COM handler, not direct shell entries
# AwesomeMenuHost will read from the existing AwesomeMenu registry structure

Write-Host "Registered AwesomeMenuHost per-user." -ForegroundColor Green
Write-Host "Restarting Explorer..." -ForegroundColor Yellow
Stop-Process -Name explorer -Force
Start-Process explorer.exe