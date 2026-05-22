Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

$guid = '{E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}'
$ok   = '[OK]  '
$miss = '[----]'
$warn = '[WARN]'

function CheckReg([string]$label, [string]$keyPath) {
    reg query "HKCU\$keyPath" 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Host "$ok $label" -ForegroundColor Green }
    else                     { Write-Host "$miss $label" -ForegroundColor DarkGray }
}

function FindOnPath([string]$exe) {
    $r = & where.exe $exe 2>$null | Select-Object -First 1
    if ($r) { return $r.Trim() }
    return ''
}

function Show([string]$label, [string[]]$paths) {
    $found = ''
    foreach ($p in $paths) {
        if ($p -and (Test-Path -LiteralPath $p -ErrorAction SilentlyContinue)) {
            $found = $p; break
        }
    }
    if ($found) {
        Write-Host "$ok $label" -ForegroundColor Green
        Write-Host "       $found" -ForegroundColor DarkCyan
    } else {
        Write-Host "$miss $label" -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "=== AwesomeMenu Diagnostics ===" -ForegroundColor Cyan

# --- Registry registrations ---
Write-Host ""
Write-Host "[Registry Registrations]" -ForegroundColor Yellow

# Read CLSID DLL path via reg query
$clsidLines = reg query "HKCU\Software\Classes\CLSID\$guid\InprocServer32" /ve 2>$null
if ($LASTEXITCODE -eq 0) {
    $dllLine = $clsidLines | Where-Object { $_ -match 'REG_SZ' } | Select-Object -First 1
    if ($dllLine -match 'REG_SZ\s+(.+)$') {
        $dll = $Matches[1].Trim()
        if (Test-Path -LiteralPath $dll -ErrorAction SilentlyContinue) {
            Write-Host "$ok CLSID registered -> $dll" -ForegroundColor Green
        } else {
            Write-Host "$warn CLSID registered but DLL missing: $dll" -ForegroundColor Yellow
        }
    } else {
        Write-Host "$warn CLSID registered but could not parse DLL path" -ForegroundColor Yellow
    }
} else {
    Write-Host "$miss CLSID not registered (run register.ps1)" -ForegroundColor Red
}

CheckReg 'Directory\Background context'  'Software\Classes\Directory\Background\shellex\ContextMenuHandlers\AwesomeMenuHost'
CheckReg 'Directory context'             'Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost'
CheckReg 'Folder context'               'Software\Classes\Folder\shellex\ContextMenuHandlers\AwesomeMenuHost'
CheckReg '* (all files) context'        'Software\Classes\*\shellex\ContextMenuHandlers\AwesomeMenuHost'
CheckReg 'AllFileSystemObjects context' 'Software\Classes\AllFileSystemObjects\shellex\ContextMenuHandlers\AwesomeMenuHost'
CheckReg 'Drive context'                'Software\Classes\Drive\shellex\ContextMenuHandlers\AwesomeMenuHost'

reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" /v $guid 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) {
    Write-Host "$ok In HKLM Approved list" -ForegroundColor Green
} else {
    Write-Host "$warn Not in HKLM Approved list (Drive context may be blocked; needs admin to fix)" -ForegroundColor Yellow
}

# --- Tool detection ---
Write-Host ""
Write-Host "[Tool Detection]" -ForegroundColor Yellow

$local = [Environment]::GetFolderPath('LocalApplicationData')
$pf    = [Environment]::GetFolderPath('ProgramFiles')
$pfx86 = [Environment]::GetFolderPath('ProgramFilesX86')
$jbScripts = "$local\JetBrains\Toolbox\scripts"

Show 'Windows Terminal'     @((FindOnPath 'wt.exe'))
Show 'PowerShell 7'        @((FindOnPath 'pwsh.exe'), "$pf\PowerShell\7\pwsh.exe")
Show 'VS Code Insiders'    @((FindOnPath 'code-insiders.exe'), "$local\Programs\Microsoft VS Code Insiders\Code - Insiders.exe")
Show 'VS Code'             @((FindOnPath 'code.exe'), "$local\Programs\Microsoft VS Code\Code.exe")
Show 'Cursor'              @((FindOnPath 'cursor.exe'), "$local\Programs\cursor\Cursor.exe", "$local\Programs\Cursor\Cursor.exe")
Show 'Zed'                 @((FindOnPath 'zed.exe'), "$local\Programs\Zed\bin\zed.exe", "$local\Programs\Zed\zed.exe")
Show 'Sublime Text'        @((FindOnPath 'subl.exe'), "$pf\Sublime Text\subl.exe", "$pfx86\Sublime Text\subl.exe", "$pf\Sublime Text 3\subl.exe")
Show 'Git'                 @((FindOnPath 'git.exe'), "$pf\Git\bin\git.exe", "$pfx86\Git\bin\git.exe")
Show 'Git Bash'            @("$pf\Git\git-bash.exe", "$pfx86\Git\git-bash.exe")
Show 'Notepad++'           @((FindOnPath 'notepad++.exe'), "$pf\Notepad++\notepad++.exe", "$pfx86\Notepad++\notepad++.exe")
Show '7-Zip'               @((FindOnPath '7z.exe'), "$pf\7-Zip\7z.exe", "$pfx86\7-Zip\7z.exe")
Show 'Python'              @((FindOnPath 'python.exe'), (FindOnPath 'py.exe'))
Show 'Node.js'             @((FindOnPath 'node.exe'), "$pf\nodejs\node.exe")
Show 'VLC'                 @((FindOnPath 'vlc.exe'), "$pf\VideoLAN\VLC\vlc.exe", "$pfx86\VideoLAN\VLC\vlc.exe")
Show 'WinMerge'            @((FindOnPath 'WinMergeU.exe'), "$pf\WinMerge\WinMergeU.exe", "$pfx86\WinMerge\WinMergeU.exe")
Show 'PyCharm (Toolbox)'  @((FindOnPath 'pycharm.cmd'), "$jbScripts\pycharm.cmd")
Show 'Rider (Toolbox)'    @((FindOnPath 'rider.cmd'), "$jbScripts\rider.cmd")
Show 'WebStorm (Toolbox)' @((FindOnPath 'webstorm.cmd'), "$jbScripts\webstorm.cmd")
Show 'CLion (Toolbox)'    @((FindOnPath 'clion.cmd'), "$jbScripts\clion.cmd")

$vsFound = $false
foreach ($ed in 'Professional','Enterprise','Community','BuildTools') {
    $devenv = "$pf\Microsoft Visual Studio\2022\$ed\Common7\IDE\devenv.exe"
    if (Test-Path -LiteralPath $devenv -ErrorAction SilentlyContinue) {
        Write-Host "$ok Visual Studio 2022 ($ed)" -ForegroundColor Green
        Write-Host "       $devenv" -ForegroundColor DarkCyan
        $vsFound = $true; break
    }
}
if (-not $vsFound) { Write-Host "$miss Visual Studio 2022" -ForegroundColor DarkGray }

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Cyan
