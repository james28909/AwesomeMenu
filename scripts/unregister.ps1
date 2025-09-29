Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$guid = '{E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}'

reg delete "HKCU\Software\Classes\CLSID\$guid" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Folder\ShellEx\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\*\shellex\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null

# Clean up HKLM system-wide entries
reg delete "HKLM\SOFTWARE\Classes\*\shellex\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null
reg delete "HKLM\SOFTWARE\Classes\Directory\Background\shellex\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null
reg delete "HKLM\SOFTWARE\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null
reg delete "HKLM\SOFTWARE\Classes\Folder\ShellEx\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null

# Also clean up old FlyoutHost entries if they exist
reg delete "HKCU\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\FlyoutHost" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\FlyoutHost" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Directory\Background\shell\FlyoutHost" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Directory\shell\FlyoutHost" /f 2>$null | Out-Null

Write-Host "Unregistered AwesomeMenuHost per-user keys." -ForegroundColor Green
Write-Host "Restarting Explorer..." -ForegroundColor Yellow
Stop-Process -Name explorer -Force
Start-Process explorer.exe