param([string]$Ext = 'txt')

# Mirrors what queryOpenWith() in AwesomeMenuHost.cpp does, plus the
# HKCU FileExts path that the C++ code currently misses.

$Ext = $Ext.TrimStart('.').ToLower()
Write-Host ""
Write-Host "=== Open-With lookup for .$Ext ===" -ForegroundColor Cyan

$blocklist = @('rundll32','dllhost','msiexec','mshta','wscript','cscript')
$seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$results = [System.Collections.Generic.List[object]]::new()

function ExeBaseName([string]$path) {
    return [System.IO.Path]::GetFileNameWithoutExtension($path)
}

function FriendlyName([string]$exeFile) {
    $key = "HKCR:\Applications\$exeFile"
    try { $v = (Get-ItemProperty $key -Name FriendlyAppName -EA Stop).FriendlyAppName; if ($v) { return $v } } catch {}
    return [System.IO.Path]::GetFileNameWithoutExtension($exeFile)
}

function ResolveExe([string]$nameOrPath) {
    if ([System.IO.Path]::IsPathRooted($nameOrPath) -and (Test-Path $nameOrPath)) { return $nameOrPath }
    $found = & where.exe $nameOrPath 2>$null | Select-Object -First 1
    if ($found) { return $found.Trim() }
    return $nameOrPath
}

function ExtractExeFromCmd([string]$cmd) {
    $cmd = $cmd.Trim()
    if ($cmd.StartsWith('"')) {
        $end = $cmd.IndexOf('"', 1)
        if ($end -gt 1) { return $cmd.Substring(1, $end - 1) }
    }
    return ($cmd -split '\s+')[0]
}

function TryAddExe([string]$exe, [string]$source) {
    if (-not $exe) { return }
    $base = ExeBaseName $exe
    if ($blocklist -contains $base.ToLower()) { return }
    if (-not $seen.Add($base)) { return }
    $friendly = FriendlyName ([System.IO.Path]::GetFileName($exe))
    $results.Add([pscustomobject]@{ DisplayName=$friendly; Exe=$exe; Source=$source })
}

function AddFromProgId([string]$progId) {
    if (-not $progId) { return }
    $cmdPath = "HKCR:\$progId\shell\open\command"
    try {
        $cmd = (Get-ItemProperty $cmdPath -Name '(default)' -EA Stop).'(default)'
        TryAddExe (ResolveExe (ExtractExeFromCmd $cmd)) "ProgID:$progId"
    } catch {}
}

# -- 1. HKCR\.<ext> default ProgID --
Write-Host "[HKCR] Checking .$Ext ..." -ForegroundColor Yellow
$hkcrExt = "HKCR:\.$Ext"
try {
    $defProgId = (Get-ItemProperty $hkcrExt -Name '(default)' -EA Stop).'(default)'
    AddFromProgId $defProgId
} catch {}

# -- 2. HKCR\.<ext>\OpenWithProgids --
try {
    $owp = Get-Item "$hkcrExt\OpenWithProgids" -EA Stop
    foreach ($name in $owp.Property) { AddFromProgId $name }
} catch {}

# -- 3. HKCR\.<ext>\OpenWithList --
try {
    $owl = Get-ChildItem "$hkcrExt\OpenWithList" -EA Stop
    foreach ($sub in $owl) {
        $exeName = $sub.PSChildName
        $appCmd = "HKCR:\Applications\$exeName\shell\open\command"
        try {
            $cmd = (Get-ItemProperty $appCmd -Name '(default)' -EA Stop).'(default)'
            TryAddExe (ResolveExe (ExtractExeFromCmd $cmd)) "HKCR\OpenWithList"
        } catch {
            TryAddExe (ResolveExe $exeName) "HKCR\OpenWithList(fallback)"
        }
    }
} catch {}

# -- 4. HKCU FileExts (per-user "Open With" history) --
Write-Host "[HKCU] Checking FileExts .$Ext ..." -ForegroundColor Yellow
$feBase = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.$Ext"

# 4a. OpenWithProgids
try {
    $feOwp = Get-Item "$feBase\OpenWithProgids" -EA Stop
    foreach ($name in $feOwp.Property) { if ($name -ne '(default)') { AddFromProgId $name } }
} catch {}

# 4b. OpenWithList — single-letter MRU keys (a, b, c...) whose values are exe filenames
try {
    $feOwlKey = Get-Item "$feBase\OpenWithList" -EA Stop
    foreach ($valName in $feOwlKey.Property) {
        if ($valName -eq 'MRUList') { continue }
        $exeName = (Get-ItemPropertyValue "$feBase\OpenWithList" -Name $valName -EA SilentlyContinue)
        if (-not $exeName -or $exeName -match '[!_]') { continue } # skip UWP identifiers
        $appCmd = "HKCR:\Applications\$exeName\shell\open\command"
        try {
            $cmd = (Get-ItemProperty $appCmd -Name '(default)' -EA Stop).'(default)'
            TryAddExe (ResolveExe (ExtractExeFromCmd $cmd)) "HKCU\FileExts\OpenWithList"
        } catch {
            TryAddExe (ResolveExe $exeName) "HKCU\FileExts\OpenWithList"
        }
    }
} catch {}

# -- 5. Universal tools (detectTools() fallback in C++ — not from registry) --
Write-Host "[Tools] Checking universal tools ..." -ForegroundColor Yellow
$pf   = [Environment]::GetFolderPath('ProgramFiles')
$pfx86 = [Environment]::GetFolderPath('ProgramFilesX86')
$hxdPaths = @((& where.exe HxD.exe 2>$null | Select-Object -First 1), "$pf\HxD\HxD.exe", "$pfx86\HxD\HxD.exe")
foreach ($p in $hxdPaths) {
    if ($p -and (Test-Path $p -EA SilentlyContinue)) {
        TryAddExe $p "detectTools(HxD)"
        break
    }
}

# -- Results --
Write-Host ""
if ($results.Count -eq 0) {
    Write-Host "No apps found - AwesomeMenu would only show Notepad fallback." -ForegroundColor Red
} else {
    Write-Host "Apps that would appear in AwesomeMenu for .$Ext :" -ForegroundColor Green
    foreach ($r in $results) {
        Write-Host ("  [{0}] {1}" -f $r.Source, $r.DisplayName) -ForegroundColor White
        Write-Host ("         $($r.Exe)") -ForegroundColor DarkCyan
    }
}
Write-Host ""
