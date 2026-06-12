# AwesomeMenuHost — Developer Documentation

## For AI Agents / Coding Assistants

If you are an AI agent or coding assistant working on this project, **read `AGENTS.md` first**.
It is the authoritative reference for build commands, deploy workflow, COM rules, known pitfalls,
and plugin system internals — all written specifically for agent consumption. This readme is
human-oriented background and overview; `AGENTS.md` is what you act on.

```
AGENTS.md  ← agent instructions, build commands, COM rules, plugin format, what NOT to do
README     ← architecture, context, human-readable overview (this file)
```

---

## Overview

AwesomeMenuHost is a **Windows Shell Extension COM DLL** that adds an unlimited cascading
right-click context menu — "Awesome Menu" — to Windows Explorer. It hooks into Explorer via
six shell contexts registered per-user under `HKEY_CURRENT_USER` (no elevation required).

**Current version**: `0.2.4` (`src/version.h`)

---

## Why This Exists: The 16-Item Problem

Windows' built-in `shell\<verb>\command` registry system hard-caps cascading menu items at
approximately 16 entries. AwesomeMenuHost sidesteps this entirely by implementing `IContextMenu3`
and building the complete menu tree in C++ at runtime. Nothing comes from Windows' own verb
parsing — every item, submenu, icon, and separator is constructed programmatically, so there is
no practical limit on depth or item count.

---

## Architecture

```
┌─────────────────┐    ┌──────────────────────┐    ┌──────────────────────┐
│  Windows        │    │  AwesomeMenuHost      │    │  Plugin System       │
│  Explorer       │◄──►│  Shell Extension      │◄──►│  (menus folder)      │
└─────────────────┘    └──────────────────────┘    └──────────────────────┘
       │                         │                           │
  User right-clicks         IContextMenu3            .reg files in
  Context menu appears      Unlimited menus          %APPDATA%\AwesomeMenuHost\menus\
                                                     Hot-reloaded every right-click
```

### Call Flow

```
right-click
  → Windows loads DLL via CLSID lookup
  → Initialize()   — captures context (what was clicked, which folder, selected files)
  → loadConfig()   — scans menus folder, parses .reg plugin files
  → QueryContextMenu()
      → createContextAwareAwesomeMenu()  — builds core menu for current context
      → folds plugin items in            — merged inside AwesomeMenu, not alongside it
      → buildCascadingMenuFixed()        — writes to HMENU, records ID→path map
  → InvokeCommand()
      → resolves relative command ID → FlyoutItem
      → expands %DIR% / %SEL% / %1 placeholders
      → ShellExecuteEx()
```

---

## Source Files

| File | Purpose |
|------|---------|
| `src/AwesomeMenuHost.h` | Core data structures (`FlyoutItem`, `Flyout`, `ContextSnapshot`, `ContextKind`) and class declaration |
| `src/AwesomeMenuHost.cpp` | All runtime logic (~2600 lines) — context detection, menu building, plugin loading, Open With lookup, command execution |
| `src/ClassFactory.h/.cpp` | COM `IClassFactory` — creates `AwesomeMenuHost` instances on demand |
| `src/Guids.h/.cpp` | CLSID definitions — **never change these values** |
| `src/dllmain.cpp` | DLL entry points; `DllRegisterServer`/`DllUnregisterServer` write/delete the 7 HKCU keys |
| `src/ExplorerCommands.h/.cpp` | `IExplorerCommand` stub — reserved for future Windows 11 modern menu work |
| `src/AwesomeMenuHost.def` | DLL exports list |
| `src/version.h` | Version constants |

---

## Data Structures

### FlyoutItem — a single executable menu entry

```cpp
struct FlyoutItem {
    std::wstring label;       // Text shown in the menu
    std::wstring command;     // Executable path passed to ShellExecuteEx as lpFile
    std::wstring args;        // Arguments passed as lpParameters; supports %DIR%, %SEL%, %1
    std::wstring icon;        // Path to exe/dll/ico for icon extraction; empty = no icon
    std::wstring workingDir;  // Typically "%DIR%" — expanded to context folder at runtime
    bool runAs = false;       // true = "runas" verb → UAC elevation prompt
    std::wstring section;     // Non-empty = separator drawn before this item's group
    std::wstring extensions;  // Semicolon-separated extensions e.g. "db;sqlite" — empty = all
};
```

### Flyout — a submenu container

```cpp
struct Flyout {
    std::wstring name;                 // Internal identifier (used in debug logs)
    std::wstring label;                // Submenu label shown in the menu
    std::wstring showIn;               // Context filter: "background;directory;file;multi"
    std::vector<FlyoutItem> items;     // Direct items in this flyout
    std::vector<Flyout> subFlyouts;    // Nested submenus — unlimited depth
};
```

### ContextSnapshot — what the user right-clicked

```cpp
struct ContextSnapshot {
    ContextKind kind;                       // Enum: Background, Directory, File, HardDrive, etc.
    std::wstring contextDir;                // Folder currently being viewed
    std::wstring selectedExt;               // Lowercase extension without dot ("py", "db", "cpp")
    std::vector<std::wstring> selection;    // Full paths of all selected items
};
```

### ContextKind — full enum

```
Background       — empty space in a folder
Directory        — a folder icon
File             — a file (generic fallback)
Multi            — multiple items selected
TextFile         — .txt .log .md .ini .cfg etc.
ImageFile        — .jpg .png .gif .bmp .ico etc.
ExecutableFile   — .exe .msi .bat .cmd .ps1 etc.
ArchiveFile      — .zip .rar .7z .tar .gz etc.
DocumentFile     — .pdf .doc .docx .xls .xlsx etc.
CodeFile         — .cpp .h .cs .js .py .java etc.
MediaFile        — .mp3 .mp4 .avi .mkv .wav etc.
HardDrive        — fixed drives (C:, D:, ...)
RemovableDrive   — USB drives, SD cards
NetworkDrive     — mapped network drives
OpticalDrive     — CD/DVD/Blu-ray
DesktopLocation  — the Desktop folder
DocumentsLocation— the Documents folder
SystemLocation   — Windows, System32, Program Files
ProjectLocation  — detected development project folders
```

---

## Menu System

### Core Menu (always present)

`createContextAwareAwesomeMenu()` builds the main flyout dynamically every right-click based on
`ContextSnapshot`. It adapts to context:

- **Folder / Background / Drive contexts**: terminals (Windows Terminal, PowerShell 7, CMD), editors
  (VS Code Insiders, VS Code, Cursor, Zed, Sublime Text, Visual Studio 2022), Git submenu
  (Git GUI, Git Bash), VS 2022 Dev Shell variants
- **File contexts**: dynamic "Open With" entries discovered from `HKCR` at runtime (see below),
  plus extension-specific actions (run `.ps1`, execute `.bat/.cmd`, extract archives, etc.)
- All detected tools are resolved once at DLL load time via `detectTools()` and cached

### Dynamic "Open With" (v0.2.4)

When a file is right-clicked, `queryOpenWith(ext)` queries `HKEY_CLASSES_ROOT` to discover every
app registered to open that extension:

1. Default ProgID from `HKCR\.<ext>`
2. `HKCR\.<ext>\OpenWithProgids` — additional ProgIDs
3. `HKCR\.<ext>\OpenWithList` — exe names

Display name comes from `HKCR\Applications\<exe.exe>\FriendlyAppName`. Results are deduplicated,
filtered (rundll32/dllhost/msiexec are blocked), and cached per extension. Label format is always
`"Open with <displayName>"` — no hardcoded app-name special-casing.

### Placeholder Expansion

Placeholders in `command`, `args`, and `workingDir` are expanded just before `ShellExecuteEx`:

| Placeholder | Expands to |
|-------------|-----------|
| `%DIR%` | The folder currently being viewed, or parent of the selection |
| `%SEL%` | Full path of the first selected item |
| `%1` | Same as `%SEL%` — standard Windows shell command syntax |

---

## Plugin System

**Location**: `%APPDATA%\AwesomeMenuHost\menus\`
(A shortcut `AppData.lnk` in the project root points there for convenience.)

Drop any `.reg` file in this folder and its items appear inside the Awesome Menu flyout on the
**next right-click** — no rebuild, no re-register, no Explorer restart needed.

Plugin items are **not** written to the Windows registry. The files are read as plain text by
`parseRegistryFileSimplified()` and their items are folded directly into the in-memory AwesomeMenu
flyout. Removing a `.reg` file removes the item on the next right-click.

### Plugin File Format

See `example_plugin.reg` in the project root for a fully commented template. Summary:

```reg
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\Classes\*\shell\MyUniqueName]
"MUIVerb"="Label shown in menu"
"Icon"="C:\\full\\path\\to\\app.exe,0"
"ext"="db;sqlite;sqlite3"

[HKEY_CURRENT_USER\Software\Classes\*\shell\MyUniqueName\command]
@="\"C:\\full\\path\\to\\app.exe\" \"%1\""
```

| Value | Required | Description |
|-------|----------|-------------|
| `MUIVerb` | Yes | Display label |
| `@=` in `\command` key | Yes | Full command line — exe is split from args automatically |
| `Icon` | No | `path\to\file.exe,0` — icon index after the comma |
| `ext` | No | Semicolon-separated extensions (no dots, lowercase). Omit to show for all contexts. |
| `HasLUAShield` | No | Present = run elevated with UAC prompt |

Paths use `\\` (standard `.reg` escaping). Both `%1` and `%SEL%` work as the selected-file
placeholder.

---

## Registration

All registration is **per-user** (`HKCU`) — no administrator rights required for the 5 main
contexts. The Drive context may additionally need an entry in the HKLM Approved list (requires
admin, one-time).

CLSID: `{E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}`

**Use the scripts — do not call regsvr32 directly:**

```powershell
# Register (kills Explorer, registers, restarts Explorer)
powershell -ExecutionPolicy Bypass -File scripts\register.ps1 -Config Release

# Unregister (removes all 7 HKCU keys, restarts Explorer)
powershell -ExecutionPolicy Bypass -File scripts\unregister.ps1

# Verify registration state and detect installed tools
powershell -ExecutionPolicy Bypass -File scripts\diagnose.ps1
```

The register script handles killing Explorer before overwriting the DLL (which Windows file-locks
while loaded) and restarting it after. Running `cmake --build` while Explorer holds the lock will
fail with `LNK1104: cannot open file 'AwesomeMenuHost.dll'`.

---

## Build & Deploy

```powershell
# 1. Kill Explorer (releases the DLL file lock)
taskkill /f /im explorer.exe

# 2. Build
cmake --build build --config Release

# 3. Register
powershell -ExecutionPolicy Bypass -File scripts\register.ps1 -Config Release

# 4. Restart Explorer
Start-Process explorer.exe
```

CMake is already configured in `build\`. Do not re-run `cmake ..` unless the CMakeLists changes.
If CMake's bundled binary is not on PATH, add it:

```powershell
$env:PATH += ";C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
```

Build output: `build\bin\Release\AwesomeMenuHost.dll`

---

## Debugging

All internal logging uses `OutputDebugStringW`. View in real time with
[Sysinternals DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview)
(filter on `AwesomeMenuHost`).

Format: `AwesomeMenuHost[Category]: message`
Categories: `General`, `Context`, `Registry`, `Menu`, `Error`

For deeper investigation:
- **Process Monitor** — filter on `explorer.exe` to see registry reads and DLL loads
- **Event Viewer** — COM activation failures appear in the Application log

---

## Scripts Reference

| Script | What it does |
|--------|-------------|
| `scripts\register.ps1` | Kills Explorer, calls regsvr32 on the built DLL, restarts Explorer. Accepts `-Config Release\|Debug`. |
| `scripts\unregister.ps1` | Removes all 7 HKCU registration keys, restarts Explorer |
| `scripts\diagnose.ps1` | Full health check — all 6 registry contexts, DLL path verification, installed tool detection |
| `scripts\verify.ps1` | Quick spot-check of the most critical registry keys |
| `scripts\test-associations.ps1` | Tests HKCR file extension → Open With lookup |

---

## Security

- **COM boundary safety**: No C++ exceptions escape `IFACEMETHODIMP` methods — all are wrapped
  in `try/catch(...)` returning `E_FAIL`. An uncaught exception in a shell extension crashes Explorer.
- **UAC integration**: `runAs = true` on a `FlyoutItem` sets the `"runas"` verb in `ShellExecuteEx`.
- **Placeholder bounds**: `expandPlaceholders()` limits replacement iterations (100 max) and string
  growth (10 KB max) to prevent abuse.
- **Plugin isolation**: Plugin `.reg` files are parsed as text only — they are never imported into
  the Windows registry by AwesomeMenu.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| Menu doesn't appear | DLL not registered or wrong path | Run `diagnose.ps1` |
| Menu appears then disappears | COM exception escaping a method | Check DebugView for `[Error]` lines |
| `LNK1104` on build | Explorer holding DLL file lock | `taskkill /f /im explorer.exe` first |
| Drive context missing | Not in HKLM Approved list | Add CLSID to Approved list as admin |
| Plugin item not showing | `ext` field doesn't match selected file extension | Check extension spelling; no dots |
| File not opening in plugin app | App receiving quoted path as literal | Strip surrounding quotes from `lpCmdLine` in the target app's `wWinMain` |
| Item shows in wrong context | `showIn` or `ext` mismatch | Review `ContextKind` detection and filter values |
