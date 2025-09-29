Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$guid = '{E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}'

Write-Host "Checking AwesomeMenuHost registration..." -ForegroundColor Cyan
reg query "HKCU\Software\Classes\CLSID\$guid\InprocServer32" 2>$null | Out-Host
reg query "HKCU\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\AwesomeMenuHost" /ve 2>$null | Out-Host
reg query "HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost" /ve 2>$null | Out-Host

Write-Host "Checking AwesomeMenu..." -ForegroundColor Cyan
reg query "HKCR\Directory\shell\AwesomeMenu" 2>$null | Out-Host

Write-Host "Done." -ForegroundColor Green