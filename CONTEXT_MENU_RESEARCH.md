# Complete Windows Context Menu Handler Research

> Validated against Microsoft Docs (Shell extensions, IExplorerCommand) and Windows 10/11 behavior as of 2025-09.

## Installation Scopes (Beyond just HKCU/HKLM)

1. **Per-User (HKCU)** - Individual user settings
2. **System-Wide (HKLM)** - All users on machine
3. **Default User Profile** - Template for new users via `HKEY_USERS\.DEFAULT`
4. **Specific User Profiles** - Via `HKEY_USERS\[SID]` for individual user accounts
5. **Policy-Enforced** - Group Policy can override both HKCU and HKLM via `Software\Policies\Microsoft\Windows\Explorer`
6. **Blocked Extensions** - Explicitly disabled extensions in `HKCU/HKLM\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Blocked`
7. **Shell Extension Approval List** - IT-managed allow-list in `HKLM\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved`

## Context Types (Beyond files, folders, drives)

1. **Files (`*`)** - All file types
2. **Folders/Directories** - Folder objects
3. **Drives** - Drive root contexts
4. **Directory Background** - Empty space in folders
5. **Library Folders** - Windows library contexts (`LibraryFolder`)
6. **Unknown File Types** - Files without known associations (`Unknown`)
7. **System File Associations** - File type groups (`SystemFileAssociations`)
8. **Namespace Objects** - Special shell namespaces (e.g., `::{20D04FE0-3AEA-1069-A2D8-08002B30309D}`)
9. **Portable Devices** - WPD contexts (phones, cameras, etc.)
10. **Network Objects** - Network shares and computers (`Network\\`)
11. **Specific File Extensions** - Extension-specific handlers (e.g., `.zip`, `.ps1`)
12. **Devices Folder** - Hardware device contexts (Device Center)
13. **Control Panel Items** - System settings contexts
14. **Windows 11 Modern Context Zones** - `ContextMenuHandlers` also applies to the compact view reached via **Shift+Right-click**

## Registry Locations for Context Menu Handlers

### Primary Shell Extension Handler Locations
- `HKCU\Software\Classes\*\ShellEx\ContextMenuHandlers`
- `HKCU\Software\Classes\Directory\ShellEx\ContextMenuHandlers`
- `HKLM\Software\Classes\*\ShellEx\ContextMenuHandlers`
- `HKLM\Software\Classes\Directory\ShellEx\ContextMenuHandlers`

> Note: `HKEY_CLASSES_ROOT` (HKCR) is a merged view of `HKCU\Software\Classes` (user precedence) and `HKLM\Software\Classes` (machine fallback).

### Specific Object Types
- `HKEY_CLASSES_ROOT\*\ShellEx\ContextMenuHandlers` (all files)
- `HKEY_CLASSES_ROOT\Folder\ShellEx\ContextMenuHandlers` (folders)
- `HKEY_CLASSES_ROOT\Directory\ShellEx\ContextMenuHandlers` (directories)
- `HKEY_CLASSES_ROOT\Drive\ShellEx\ContextMenuHandlers` (drives)
- `HKEY_CLASSES_ROOT\SystemFileAssociations\[extension]\shell\command` (file associations)
- `HKEY_CLASSES_ROOT\LibraryFolder\background\shellex\ContextMenuHandlers` (library folders)
- `HKEY_CLASSES_ROOT\Unknown\shellex\ContextMenuHandlers` (unknown file types)

### Windows 11 Modern Context Menu Registration
- `HKCU\Software\Classes\CLSID\{Guid}\InprocServer32` + `HKCU\Software\Classes\CLSID\{Guid}\Implemented Categories\{86C86720-42A0-1069-A2E8-08002B30309D}` for IExplorerCommand implementations
- `HKCU\Software\Classes\Directory\Shell\VerbName` → `CommandStateHandler`, `Icon`, `MUIVerb`, `ExplorerCommandHandler` (for Win11 compact menu and Win10 classic menu)
- `HKCU\Software\Classes\Directory\Background\Shell\VerbName` for background-only verbs
- Equivalent HKLM paths for machine-wide scope

### Static Verb Registration (Simple Context Menu Items)
- `HKEY_CLASSES_ROOT\*\shell\[VerbName]\command` (files)
- `HKEY_CURRENT_USER\Software\Classes\directory\shell\MenuItemName\command` (folders)
- `HKEY_CURRENT_USER\Software\Classes\directory\Background\shell\MenuItemName\command` (folder background)

## Special Context Behaviors

- **Extended Verbs** - Shift+right-click only (add `extended` REG_SZ with empty data)
- **Conditional Menus** - AQS (Advanced Query Syntax) based visibility using `AppliesTo` values
- **Cascading Menus** - Multi-level submenu structures via `SubCommands` or `IExplorerCommandProvider`
- **UAC-Aware** - Elevated processes only load HKLM handlers unless `RunAs` metadata is present
- **HasLUAShield** - Shows UAC shield icon when `HasLUAShield` REG_SZ = ""
- **Modern vs Classic Surface** - Windows 11 displays compact classic menu when Shift is held; ensure handlers work in both layers

## IContextMenu Interface Progression

- **IContextMenu**: Basic context menu (16 item limit)
- **IContextMenu2**: Adds owner-drawn menu support
- **IContextMenu3**: Adds keyboard navigation, tooltips, and **can bypass the 16 item limit**

## Registry Cache System

`HKEY_USERS\.DEFAULT\Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Cached` is Windows' performance cache for shell extensions. It stores:
- Previously loaded extension metadata
- CLSID combinations for faster lookup
- User-specific shell extension state
- Helps Explorer avoid re-parsing registry on every right-click

This cache can become corrupted and cause shell extension issues, making it important for troubleshooting.

> To reset cache safely: remove the `Cached` subkey and restart Explorer (`taskkill /IM explorer.exe /F` then `start explorer.exe`).

## COM Apartment Threading

COM apartment threading determines how COM objects handle multiple threads:
- **Single Threaded Apartment (STA)**: One thread at a time, safer but slower
- **Multi Threaded Apartment (MTA)**: Multiple threads, faster but requires thread safety
- **ThreadingModel = Apartment** in registry means STA

For elevated processes, Windows is more restrictive about loading extensions due to security.

## Registry Precedence Rules

- HKCU entries take precedence over HKLM for most classes due to HKCR merge ordering
- Group Policy (`Software\Policies\Microsoft\Windows\Explorer`) can disable or enforce handlers regardless of HKCU/HKLM values
- UAC elevated processes load only HKLM handlers unless the extension is explicitly registered under `HKLM\Software\Classes` and marked safe for elevation
- Blocked extensions in `Shell Extensions\Blocked` override other registrations; allow-list (`Shell Extensions\Approved`) must include CLSID for Win11 compact menu

## Troubleshooting

### Common Issues
- Menu truncation in classic menu when exceeding 16 items per cascade (IContextMenu3 bypasses this)
- Shell extension conflicts between HKCU and HKLM registrations (duplicate CLSIDs)
- Cache corruption causing extensions not to load
- File association conflicts with existing handlers or application-provided verbs
- Windows 11 modern menu ignoring classic-only registrations (missing `ExplorerCommandHandler`)

### Solutions
- Use IContextMenu3 for unlimited submenu generation
- Implement programmatic menu building instead of relying on Windows registry parsing
- Clear shell extension cache when troubleshooting
- Use proper COM apartment threading for compatibility
- Ensure `ExplorerCommandHandler` is present for Windows 11 modern surface when using `IExplorerCommand`

## Advanced Context Detection & Menu Adaptation

### Context-Aware Shell Extensions
Modern shell extensions can dynamically adapt their content based on what the user right-clicked:

**Context Types Detected:**
- **Background (0)**: Empty space in folders
- **Directory (1)**: Folder objects
- **File (2)**: Generic file
- **Multi (3)**: Multiple selected items
- **TextFile (4)**: .txt, .log, .md, .ini, .cfg files
- **ImageFile (5)**: .jpg, .png, .gif, .bmp, .ico files
- **ExecutableFile (6)**: .exe, .msi, .bat, .cmd, .ps1 files
- **ArchiveFile (7)**: .zip, .rar, .7z, .tar, .gz files
- **DocumentFile (8)**: .pdf, .doc, .docx, .xls, .xlsx files
- **CodeFile (9)**: .cpp, .h, .cs, .js, .py, .java files
- **MediaFile (10)**: .mp3, .mp4, .avi, .mkv, .wav files
- **HardDrive (11)**: Fixed drives (C:, D:)
- **RemovableDrive (12)**: USB drives, SD cards
- **NetworkDrive (13)**: Mapped network drives
- **OpticalDrive (14)**: CD/DVD/Blu-ray drives
- **DesktopLocation (15)**: Desktop folder
- **DocumentsLocation (16)**: Documents folder
- **SystemLocation (17)**: Windows, System32, Program Files
- **ProjectLocation (18)**: Detected development folders
- **PinnedFolder (19)**: Quick Access / pinned contexts
- **RecycleBin (20)**: Items inside the Recycle Bin (requires `CLSID_{645FF040-5081-101B-9F08-00AA002F954E}`)

### Hybrid Configuration Systems

**Static + Dynamic Approach:**
- **Always Present Core Menu**: Reliable base functionality that always appears
- **Context-Adaptive Content**: Menu items change based on detected context
- **User-Extensible Registry Files**: Additional flyouts from .reg files in `%APPDATA%\AwesomeMenuHost\menus\`

This enables unlimited customization without recompilation while maintaining consistent core functionality.

### Registry Registration Persistence

All shell extension registrations survive reboots and are persistent:

**COM Object Registration:**
```
HKCU\Software\Classes\CLSID\{GUID}\InprocServer32
├── (Default) = "C:\path\to\extension.dll"
└── ThreadingModel = "Apartment"
```

**Context Menu Handler Registration:**
```
HKCU\Software\Classes\*\shellex\ContextMenuHandlers\ExtensionName
HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\ExtensionName
HKCU\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\ExtensionName
HKCU\Software\Classes\Folder\ShellEx\ContextMenuHandlers\ExtensionName
```

### Debug Output Considerations

**Windows-Specific Format Specifiers:**
- Use `%Iu` instead of `%zu` for `size_t` in `wsprintfW()`
- Windows doesn't support C99 format specifiers in COM contexts
- Debug output via `OutputDebugStringW()` is visible in DebugView

### Performance Optimizations

**Minimize loadConfig() Calls:**
- Context detection happens in `QueryContextMenu()`, not `Initialize()`
- Create menus dynamically rather than pre-loading all possible combinations
- Cache expensive operations like file type detection

## Best Practices

1. **For Development**: Use HKCU registration (no admin required, easier testing)
2. **For Production**: Provide installer choice (HKCU vs HKLM) and document elevation requirements
3. **For Enterprise**: Use HKLM with Group Policy support and populate `Shell Extensions\Approved`
4. **For Unlimited Menus**: Use IContextMenu3 or IExplorerCommand with owner-drawn UI
5. **For Security**: Validate all file paths and commands before execution; opt-in to the approval list when required
6. **For Context Awareness**: Implement dynamic menu creation in `QueryContextMenu()` using captured context snapshots
7. **For Extensibility**: Support both static core functionality and user-configurable additions (e.g., `%APPDATA%\AwesomeMenuHost\menus`)
8. **For Debugging**: Use Windows-compatible format specifiers and `OutputDebugStringW()` or ETW logging
9. **For Performance**: Avoid creating all possible menus upfront, generate on-demand, and cache expensive lookups
10. **For Reliability**: Handle Explorer restarts gracefully (extensions reload automatically); include self-healing registry cleanup
