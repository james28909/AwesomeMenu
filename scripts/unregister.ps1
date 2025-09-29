Param(
	[switch]$KeepExplorer
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$guid = '{E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}'

$pathsToRemove = @(
	"HKCU:\Software\Classes\CLSID\$guid",
	'HKCU:\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\AwesomeMenuHost',
	'HKCU:\Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost',
	'HKCU:\Software\Classes\Folder\shellex\ContextMenuHandlers\AwesomeMenuHost',
	'HKCU:\Software\Classes\*\shellex\ContextMenuHandlers\AwesomeMenuHost',
	'HKCU:\Software\Classes\AllFileSystemObjects\shellex\ContextMenuHandlers\AwesomeMenuHost',
	'HKCU:\Software\Classes\Drive\shellex\ContextMenuHandlers\AwesomeMenuHost'
)

foreach ($path in $pathsToRemove) {
	if (Test-Path $path) {
		Remove-Item -Path $path -Recurse -Force
	}
}

$legacyCleanup = @(
	'HKCU:\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\FlyoutHost',
	'HKCU:\Software\Classes\Directory\shellex\ContextMenuHandlers\FlyoutHost',
	'HKCU:\Software\Classes\Directory\Background\shell\FlyoutHost',
	'HKCU:\Software\Classes\Directory\shell\FlyoutHost'
)

foreach ($path in $legacyCleanup) {
	if (Test-Path $path) {
		Remove-Item -Path $path -Recurse -Force
	}
}

Write-Host "Unregistered AwesomeMenuHost per-user keys." -ForegroundColor Green

if (-not $KeepExplorer) {
	Write-Host "Restarting Explorer..." -ForegroundColor Yellow
	Start-Process -FilePath 'taskkill' -ArgumentList '/F','/IM','explorer.exe' -NoNewWindow -Wait
	Start-Process explorer.exe
}