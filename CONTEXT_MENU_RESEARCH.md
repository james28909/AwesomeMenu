# Complete Windows Context Menu Handler Research

## Installation Scopes (Beyond just HKCU/HKLM)

1. **Per-User (HKCU)** - Individual user settings
2. **System-Wide (HKLM)** - All users on machine
3. **Default User Profile** - Template for new users via `HKEY_USERS\.DEFAULT`
4. **Specific User Profiles** - Via `HKEY_USERS\[SID]` for individual user accounts
5. **Policy-Enforced** - Group Policy can override both HKCU and HKLM
6. **Blocked Extensions** - Explicitly disabled extensions in special "Blocked" registry keys

## Context Types (Beyond files, folders, drives)

1. **Files (`*`)** - All file types
2. **Folders/Directories** - Folder objects
3. **Drives** - Drive root contexts
4. **Directory Background** - Empty space in folders
5. **Library Folders** - Windows library contexts
6. **Unknown File Types** - Files without known associations
7. **System File Associations** - File type groups
8. **Namespace Objects** - Special shell namespaces
9. **Portable Devices** - WPD contexts (phones, cameras, etc.)
10. **Network Objects** - Network shares and computers
11. **Specific File Extensions** - Extension-specific handlers
12. **Devices Folder** - Hardware device contexts
13. **Control Panel Items** - System settings contexts

## Registry Locations for Context Menu Handlers

### Primary Shell Extension Handler Locations
- `HKCU\Software\Classes\*\ShellEx\ContextMenuHandlers`
- `HKCU\Software\Classes\Directory\ShellEx\ContextMenuHandlers`
- `HKLM\Software\Classes\*\ShellEx\ContextMenuHandlers`
- `HKLM\Software\Classes\Directory\ShellEx\ContextMenuHandlers`

### Specific Object Types
- `HKEY_CLASSES_ROOT\*\ShellEx\ContextMenuHandlers` (all files)
- `HKEY_CLASSES_ROOT\Folder\ShellEx\ContextMenuHandlers` (folders)
- `HKEY_CLASSES_ROOT\Directory\ShellEx\ContextMenuHandlers` (directories)
- `HKEY_CLASSES_ROOT\Drive\ShellEx\ContextMenuHandlers` (drives)
- `HKEY_CLASSES_ROOT\SystemFileAssociations\[extension]\shell\command` (file associations)
- `HKEY_CLASSES_ROOT\LibraryFolder\background\shellex\ContextMenuHandlers` (library folders)
- `HKEY_CLASSES_ROOT\Unknown\shellex\ContextMenuHandlers` (unknown file types)

### Static Verb Registration (Simple Context Menu Items)
- `HKEY_CLASSES_ROOT\*\shell\[VerbName]\command` (files)
- `HKEY_CURRENT_USER\Software\Classes\directory\shell\MenuItemName\command` (folders)
- `HKEY_CURRENT_USER\Software\Classes\directory\Background\shell\MenuItemName\command` (folder background)

## Special Context Behaviors

- **Extended Verbs** - Shift+right-click only (add "extended" REG_SZ value)
- **Conditional Menus** - AQS (Advanced Query Syntax) based visibility using "AppliesTo" values
- **Cascading Menus** - Multi-level submenu structures via "SubCommands"
- **UAC-Aware** - Different behavior for elevated processes
- **HasLUAShield** - Shows UAC shield icon, indicates elevation needed

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

## COM Apartment Threading

COM apartment threading determines how COM objects handle multiple threads:
- **Single Threaded Apartment (STA)**: One thread at a time, safer but slower
- **Multi Threaded Apartment (MTA)**: Multiple threads, faster but requires thread safety
- **ThreadingModel = Apartment** in registry means STA

For elevated processes, Windows is more restrictive about loading extensions due to security.

## Registry Precedence Rules

- HKCU entries take precedence over HKLM for most settings
- Policy enforcement often prioritizes machine-wide (HKLM) settings
- UAC elevated processes will not load extensions from HKCU due to security concerns
- Blocked extensions in special registry keys override other registrations

## Troubleshooting

### Common Issues
- Menu truncation due to Windows limits on cascading menus
- Shell extension conflicts between HKCU and HKLM registrations
- Cache corruption causing extensions not to load
- File association conflicts with existing handlers

### Solutions
- Use IContextMenu3 for unlimited submenu generation
- Implement programmatic menu building instead of relying on Windows registry parsing
- Clear shell extension cache when troubleshooting
- Use proper COM apartment threading for compatibility

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
2. **For Production**: Consider GUI installer with user choice of scope
3. **For Enterprise**: Use HKLM with Group Policy support
4. **For Unlimited Menus**: Use IContextMenu3 with programmatic generation
5. **For Security**: Validate all file paths and commands before execution
6. **For Context Awareness**: Implement dynamic menu creation in QueryContextMenu()
7. **For Extensibility**: Support both static core functionality and user-configurable additions
8. **For Debugging**: Use Windows-compatible format specifiers and OutputDebugStringW()
9. **For Performance**: Avoid creating all possible menus upfront, generate on-demand
10. **For Reliability**: Handle Explorer restarts gracefully (extensions reload automatically)