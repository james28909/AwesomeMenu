Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$guid = '{E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}'

# Remove COM class registration
reg delete "HKCU\Software\Classes\CLSID\$guid" /f 2>$null | Out-Null

# Remove the 6 context handler registrations (mirrors register.ps1 and DllRegisterServer)
reg delete "HKCU\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost"            /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Folder\shellex\ContextMenuHandlers\AwesomeMenuHost"               /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\*\shellex\ContextMenuHandlers\AwesomeMenuHost"                    /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\AllFileSystemObjects\shellex\ContextMenuHandlers\AwesomeMenuHost" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Drive\shellex\ContextMenuHandlers\AwesomeMenuHost"                /f 2>$null | Out-Null

# Legacy cleanup (old names/locations from previous versions)
reg delete "HKCU\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\FlyoutHost" /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\FlyoutHost"            /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Directory\Background\shell\FlyoutHost"                       /f 2>$null | Out-Null
reg delete "HKCU\Software\Classes\Directory\shell\FlyoutHost"                                  /f 2>$null | Out-Null

Write-Host "Unregistered AwesomeMenuHost per-user keys." -ForegroundColor Green
Write-Host "Restarting Explorer..." -ForegroundColor Yellow
Stop-Process -Name explorer -Force
Start-Process explorer.exe