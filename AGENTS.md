# AGENTS.md — AwesomeMenuHost

Agent-focused documentation for the AwesomeMenu Windows Shell Extension project.
This file covers what agents need to know to make correct, safe changes to this codebase.

---

## What This Project Is

AwesomeMenuHost is a **Windows Shell Extension COM DLL** (`AwesomeMenuHost.dll`) that adds an
unlimited cascading right-click context menu ("Awesome Menu") to Windows Explorer. It hooks into
Explorer via 6 shell contexts registered under `HKEY_CURRENT_USER` (no elevation required for
registration).

**Why IContextMenu3 and not the registry verb system?**
Windows' built-in `shell\<verb>\command` registry parsing is hard-capped at ~16 cascading items.
AwesomeMenuHost implements `IContextMenu3` and builds the entire menu tree in C++ at runtime,
completely bypassing that limit. Every menu item, submenu, icon, and section separator is built
programmatically — nothing comes from Windows' own verb parsing.

---

## Environment Requirements

- **OS**: Windows 10/11, x64 only (Explorer is 64-bit; the build enforces this via CMake)
- **Compiler**: MSVC (Visual Studio 2022 Professional) — clang/gcc are not supported; COM
  registration and shell APIs are MSVC-specific in this project
- **CMake**: `cmake` must be on PATH. If using VS2022's bundled CMake:
  ```powershell
  $env:PATH += ";C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
  ```
- **Windows SDK**: 10.0.26100.0 (set in CMake cache; do not change without testing)
- **C++ Standard**: C++20 (`std::format`, `std::wstring`, `std::optional` are all used)
- **Build target**: x64 Release for production, Debug for local iteration with DebugView

---

## Build System

The project uses CMake with an MSVC generator. The build directory is pre-configured at `build\`.
**Do not re-run `cmake ..` without good reason** — the build directory is already bootstrapped and
contains the `.vcxproj` files Explorer's lock tracking depends on.

```powershell
# Standard build (Release)
cmake --build build --config Release

# Standard build (Debug — use with Sysinternals DebugView)
cmake --build build --config Debug
```

**Output**: `build\bin\Release\AwesomeMenuHost.dll` (or `Debug\`)
**Import lib**: `build\lib\Release\AwesomeMenuHost.lib`

CMakeLists.txt key facts:
- Target name is `comtext_flyout` (legacy internal name); output is renamed to `AwesomeMenuHost`
- Links: `shlwapi`, `shell32`, `ole32`, `uuid`
- Compile definitions: `UNICODE`, `_UNICODE`, `_WIN32_WINNT=0x0A00`
- x64 is enforced: `if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8) message(FATAL_ERROR ...)`

---

## Deploy / Register Workflow

**The DLL must not be in use by Explorer when the file is written.** Explorer holds a file lock on
loaded shell extension DLLs. Always kill Explorer before building, then restart it after.

```powershell
# Full cycle (kill → build → register → restart)
taskkill /f /im explorer.exe
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File scripts\register.ps1 -Config Release
Start-Process explorer.exe
```

`register.ps1` calls `regsvr32` which invokes `DllRegisterServer` inside the DLL. This writes
7 HKCU registry keys:

| Key | Purpose |
|-----|---------|
| `HKCU\Software\Classes\CLSID\{guid}\InprocServer32` | Points Windows to the DLL file |
| `HKCU\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\AwesomeMenuHost` | Empty folder space right-click |
| `HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost` | Folder icon right-click |
| `HKCU\Software\Classes\Folder\shellex\ContextMenuHandlers\AwesomeMenuHost` | Folder object right-click |
| `HKCU\Software\Classes\*\shellex\ContextMenuHandlers\AwesomeMenuHost` | All files right-click |
| `HKCU\Software\Classes\AllFileSystemObjects\shellex\ContextMenuHandlers\AwesomeMenuHost` | All filesystem objects |
| `HKCU\Software\Classes\Drive\shellex\ContextMenuHandlers\AwesomeMenuHost` | Drive root right-click |

**HKLM Approved list**: The Drive context may be blocked without an entry in
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved`. Adding it requires
admin. CLSID is `{E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}`.

To remove everything:
```powershell
powershell -ExecutionPolicy Bypass -File scripts\unregister.ps1
```

---

## Diagnostic Scripts

All scripts are in `scripts\`. None of them modify the build or source files.

| Script | Purpose |
|--------|---------|
| `diagnose.ps1` | Full health check — verifies all 6 HKCU contexts, checks DLL path exists, detects installed tools (WT, VS Code, Git, etc.) |
| `register.ps1` | Kills Explorer, calls regsvr32, restarts Explorer. Accepts `-Config Release\|Debug` |
| `unregister.ps1` | Removes all 7 HKCU keys and restarts Explorer |
| `verify.ps1` | Quick registry spot-check (subset of diagnose) |
| `test-associations.ps1` | Tests file extension → Open With registry lookup |

Run diagnostics:
```powershell
powershell -ExecutionPolicy Bypass -File scripts\diagnose.ps1
```

---

## Source File Map

```
src/
├── AwesomeMenuHost.h       Core data structures and class declaration
├── AwesomeMenuHost.cpp     All runtime logic (~2600 lines) — read this first
├── ClassFactory.h/.cpp     COM IClassFactory — boilerplate, rarely needs changes
├── Guids.h/.cpp            CLSID definitions — never change the GUID values
├── dllmain.cpp             DLL entry points + DllRegisterServer/DllUnregisterServer
├── ExplorerCommands.h/.cpp IExplorerCommand stub — reserved for future Win11 work
└── AwesomeMenuHost.def     DLL exports list
```

### Key Data Structures (`AwesomeMenuHost.h`)

```cpp
struct FlyoutItem {
    std::wstring label;       // Text shown in menu
    std::wstring command;     // Executable path (passed to ShellExecuteEx lpFile)
    std::wstring args;        // Arguments (passed to lpParameters); supports %1/%SEL%/%DIR%
    std::wstring icon;        // Exe/dll/ico path for icon extraction; empty = no icon
    std::wstring workingDir;  // Usually "%DIR%" which expands at runtime
    bool runAs = false;       // true = "runas" verb → UAC elevation
    std::wstring section;     // Non-empty = separator before this item's group
    std::wstring extensions;  // Semicolon list e.g. "db;sqlite" — only show for these exts
};

struct Flyout {
    std::wstring name;                 // Internal identifier (used in logs)
    std::wstring label;                // Submenu label shown in menu
    std::wstring showIn;               // Filter: "background;directory;file;multi" (empty = all)
    std::vector<FlyoutItem> items;     // Direct items in this flyout
    std::vector<Flyout> subFlyouts;    // Nested submenus (unlimited depth)
};

struct ContextSnapshot {
    ContextKind kind;                  // What was right-clicked (see ContextKind enum)
    std::wstring contextDir;           // The directory in context (folder being viewed)
    std::wstring selectedExt;          // Lowercase extension without dot ("py", "db") — single file only
    std::vector<std::wstring> selection; // Full paths of all selected items
};
```

### ContextKind Enum

Describes what the user right-clicked. Set in `Initialize()` via `detectContextKind()`:

```
Background, Directory, File, Multi
TextFile, ImageFile, ExecutableFile, ArchiveFile, DocumentFile, CodeFile, MediaFile
HardDrive, RemovableDrive, NetworkDrive, OpticalDrive
DesktopLocation, DocumentsLocation, SystemLocation, ProjectLocation
```

---

## Runtime Flow (Every Right-Click)

```
Explorer right-click
  → Windows looks up CLSID in HKCU
  → Loads AwesomeMenuHost.dll (if not already loaded)
  → DllGetClassObject → ClassFactory → new AwesomeMenuHost()
  → Initialize(pidlFolder, pdtobj)
      → buildContextSnapshot()     — extracts contextDir + selection paths + selectedExt
      → detectContextKind()        — classifies the click (Background/File/Directory/etc.)
      → loadConfig()
          → loadRegistryFilesAsSeparateFlyouts()
              → scans %APPDATA%\AwesomeMenuHost\menus\*.reg
              → parseRegistryFileSimplified() per file → populates m_flyouts
  → QueryContextMenu(hMenu, ...)
      → createContextAwareAwesomeMenu(snapshot) → builds the main Flyout in memory
      → folds m_flyouts items into contextAware (ext filtering applied here)
      → buildCascadingMenuFixed() — inserts items into HMENU, records ID→path map
  → [User clicks item]
  → InvokeCommand(pici)
      → look up relative ID in m_idToPath
      → findItemByPath() → returns FlyoutItem*
      → expandPlaceholders() — replaces %DIR%, %SEL%, %1 with real paths
      → ShellExecuteEx(lpFile=command, lpParameters=args)
```

**Critical ID mapping detail**: Windows passes absolute command IDs (e.g. 31060) when building
the menu but sends *relative* IDs (e.g. 0) when invoking. AwesomeMenuHost stores
`id - idCmdFirst` in `m_idToPath` to handle this. Do not change this without understanding the
full mapping chain in `buildCascadingMenuFixed` and `InvokeCommand`.

---

## Placeholder Expansion

`expandPlaceholders()` is called on `command`, `args`, and `workingDir` just before
`ShellExecuteEx`. Supported tokens:

| Token | Expands to |
|-------|-----------|
| `%DIR%` | `m_context.contextDir` — the folder being viewed or parent of selection |
| `%SEL%` | `m_context.primarySelection()` — full path of the first selected item |
| `%1` | Same as `%SEL%` — standard Windows shell command placeholder |

If `%SEL%`/`%1` is used but no file is selected (e.g. background right-click), the token is left
unexpanded. Design menu items accordingly — use `%DIR%` for folder contexts, `%SEL%`/`%1` only
when a file is expected.

---

## Dynamic "Open With" System (v0.2.4+)

When a file is right-clicked, `createContextAwareAwesomeMenu` calls `queryOpenWith(ext)` which
queries `HKCR` at runtime to discover all apps registered to open that extension:

1. `HKCR\.<ext>` default value → default ProgID
2. `HKCR\.<ext>\OpenWithProgids` → additional ProgIDs
3. `HKCR\.<ext>\OpenWithList` → exe names

For each discovered app it reads `HKCR\Applications\<exe.exe>\FriendlyAppName` for the display
name. Results are deduplicated, blocklisted (`rundll32`, `dllhost`, `msiexec` are excluded), and
cached per extension for the DLL's lifetime.

Label format: `"Open with " + displayName` — no special-casing per app name.

---

## Plugin System — menus folder

**Location**: `%APPDATA%\AwesomeMenuHost\menus\` (shortcut `AppData.lnk` in project root)

Any `.reg` file dropped here is picked up on the **next right-click** — no rebuild or re-register
needed. Items from these files are folded directly into the AwesomeMenu flyout (not shown as
separate context menu entries).

### Plugin .reg File Format

Use `example_plugin.reg` in the project root as a template. Key fields:

```reg
Windows Registry Editor Version 5.00

; The key path after \shell\ is an internal unique name — not the displayed label.
; The HKEY_CURRENT_USER\Software\Classes\*\shell\ prefix is just convention;
; AwesomeMenu's parser only cares about \shell\ appearing somewhere in the path.
[HKEY_CURRENT_USER\Software\Classes\*\shell\MyUniqueName]
"MUIVerb"="Label shown in menu"
"Icon"="C:\\full\\path\\to\\app.exe,0"
"ext"="db;sqlite;sqlite3"

[HKEY_CURRENT_USER\Software\Classes\*\shell\MyUniqueName\command]
@="\"C:\\full\\path\\to\\app.exe\" \"%1\""
```

**Supported values**:

| Value | Required | Description |
|-------|----------|-------------|
| `MUIVerb` | Yes | Display label in the menu |
| `@=` (default of `\command` key) | Yes | Full command line; exe is split from args automatically |
| `Icon` | No | Path to exe/dll/ico with optional `,<index>` |
| `ext` | No | Semicolon-separated lowercase extensions (no dots). If omitted, item shows for all contexts. |
| `HasLUAShield` | No | Any value — shows UAC shield and runs elevated |

**Placeholders in the command**: `%1` or `%SEL%` = selected file path; `%DIR%` = context folder.
Use `\"%1\"` (with quotes) to handle paths with spaces.

**Backslash escaping**: paths use `\\` (standard `.reg` format). The parser unescapes `\\`→`\`
and `\"`→`"` before use.

**The `ext` filter** is evaluated against `ContextSnapshot.selectedExt` (lowercase, no dot).
If the user right-clicks on a background or folder, `selectedExt` is empty and any item with
`ext` set will be hidden.

**The files are NOT imported into the Windows registry.** They are read as plain text by
`parseRegistryFileSimplified()`. The key paths are only used to locate `\shell\` in the line
and extract the command name — they have no effect on Windows itself.

---

## Logging / Debugging

All logging goes through `logDebug(message, category)` which calls `OutputDebugStringW`.
View output in real time with **Sysinternals DebugView** (filter on `AwesomeMenuHost`).

Log categories: `General`, `Context`, `Registry`, `Menu`, `Error`

Output format: `AwesomeMenuHost[Category]: message`

**Important format specifier rule**: Use `%Iu` not `%zu` for `size_t` in `wsprintfW`. Windows
does not support C99 `%zu` in COM-hosted contexts. Use `std::format` (C++20) where possible —
it handles sizes correctly.

---

## COM Rules — Read Before Touching COM Methods

1. **No C++ exceptions may escape COM method boundaries.** Every `IFACEMETHODIMP` is wrapped in
   `try { ... } catch (...) { return E_FAIL; }`. This is mandatory — an exception reaching
   Explorer crashes the shell.

2. **Reference counting must be symmetric.** `AddRef`/`Release` on `AwesomeMenuHost` increment/
   decrement `m_ref`. When `m_ref` hits 0, the object deletes itself. The global `g_cDllRef`
   tracks total live objects; `DllCanUnloadNow` returns `S_OK` only when it's 0.

3. **Threading model is `Apartment` (STA).** Shell extension objects are created and used on the
   same thread. No cross-thread calls to COM objects.

4. **`m_context` lifetime**: Set in `Initialize()`, valid through `InvokeCommand()`. Do not cache
   the `ContextSnapshot` across calls.

5. **Icon bitmaps** (`m_menuBitmaps`) are allocated in `QueryContextMenu` and freed in
   `HandleMenuMsg2` when `WM_UNINITMENUPOPUP` fires. Never free them elsewhere.

---

## What NOT to Do

- **Do not change the CLSID** in `Guids.h/cpp`. If you do, the old registration becomes an
  orphan that Windows will try to load and fail, showing a broken shell extension. Run
  `unregister.ps1` first, change the GUID, then `register.ps1`.

- **Do not add `applyRegistryFile` calls back** to `loadRegistryFilesAsSeparateFlyouts`. It was
  intentionally removed because it wrote `HKCU\Software\Classes\*\shell\*` keys which caused
  plugin items to appear both inside AwesomeMenu AND as standalone context menu entries (double
  entry bug). The menus folder is read-only from the registry's perspective.

- **Do not call `loadConfig()` from anywhere other than `Initialize()`**. It rebuilds all flyout
  state and is called fresh on every right-click already.

- **Do not use `printf`-style format strings with `%zu`** in `wsprintfW`. Use `%Iu` or switch to
  `std::format`.

- **Do not push to `m_flyouts` after `buildContextMenu` starts**. The fold loop that merges
  plugin items into the AwesomeMenu flyout runs at the top of `buildContextMenu`; anything pushed
  to `m_flyouts` after that point is ignored for the current invocation.

- **Do not build for x86.** CMake will error out, but if you bypass it, Explorer will silently
  refuse to load the DLL.

---

## Adding a New Context Type

1. Add a value to the `ContextKind` enum in `AwesomeMenuHost.h`
2. Add detection logic in `detectContextKind()` in `AwesomeMenuHost.cpp`
3. Add a `matchShowIn` case in `buildContextMenu` if the new kind needs custom `showIn` tokens
4. Add menu-building logic in `createContextAwareAwesomeMenu` under the appropriate `if/else`
   branch

---

## Adding a New Tool to Auto-Detection

Tools are detected once at DLL load time via the static lambda in `detectTools()`. Results are
cached in `s_cached` for the DLL's lifetime (Explorer restart resets the cache).

To add a new tool:
1. Add a field to `ToolPaths` struct (anonymous namespace in `AwesomeMenuHost.cpp`)
2. Add detection in the static initializer lambda inside `detectTools()` using `findOnPath()` and
   direct path checks
3. Reference it in `createContextAwareAwesomeMenu` via `tools.<field>`
4. Add the same path candidates to `scripts\diagnose.ps1` under `[Tool Detection]`

---

## Version

`src/version.h` — bump `PATCH` for fixes, `MINOR` for new features, `MAJOR` for breaking changes.
Currently: `0.2.4`.

---

## Commit Policy

Complete a full working iteration before committing. If a commit introduces a regression, revert
it with a clear explanation in the commit message rather than patching forward. One commit per
feature/fix iteration, not per edit.
