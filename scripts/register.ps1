Param(
  [ValidateSet('Debug','Release')]
  [string]$Config = 'Debug',
  [switch]$Rebuild,
  [switch]$NoRestart
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

# Helper to set a registry value and emit detail when running in verbose consoles
function Set-RegistryString {
  param(
    [Parameter(Mandatory)][string]$Path,
    [string]$Name,
    [Parameter(Mandatory)][string]$Value
  )

  $null = New-Item -Path $Path -Force
  Set-ItemProperty -Path $Path -Name $Name -Value $Value -Type String
}

$clsidRoot = "HKCU:\Software\Classes\CLSID\$guid"
Set-RegistryString -Path $clsidRoot -Name '(default)' -Value 'AwesomeMenuHost Shell Extension'
Set-RegistryString -Path "$clsidRoot\InprocServer32" -Name '(default)' -Value $dll
Set-RegistryString -Path "$clsidRoot\InprocServer32" -Name 'ThreadingModel' -Value 'Apartment'

# Register handler across standard shell contexts
$handlerTargets = @(
  'HKCU:\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\AwesomeMenuHost',
  'HKCU:\Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost',
  'HKCU:\Software\Classes\Folder\shellex\ContextMenuHandlers\AwesomeMenuHost',
  'HKCU:\Software\Classes\*\shellex\ContextMenuHandlers\AwesomeMenuHost',
  'HKCU:\Software\Classes\AllFileSystemObjects\shellex\ContextMenuHandlers\AwesomeMenuHost',
  'HKCU:\Software\Classes\Drive\shellex\ContextMenuHandlers\AwesomeMenuHost'
)

foreach ($target in $handlerTargets) {
  Set-RegistryString -Path $target -Name '(default)' -Value $guid
}

Write-Host "Registered AwesomeMenuHost per-user." -ForegroundColor Green

if (-not $NoRestart) {
  Write-Host "Restarting Explorer..." -ForegroundColor Yellow
  Start-Process -FilePath 'taskkill' -ArgumentList '/F','/IM','explorer.exe' -NoNewWindow -Wait
  Start-Process explorer.exe
}