# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

AwesomeMenuHost is a Windows Shell Extension that **bypasses Windows' 16-item cascading menu limitation** using IContextMenu3 with programmatic menu generation. This is the key technical breakthrough - while registry-based shell extensions are limited by Windows' internal parsing, this extension builds unlimited menus entirely in code.

## Build Commands

```bash
# Build (requires Visual Studio 2019/2022)
cmake --build build --config Debug
cmake --build build --config Release

# Register shell extension for testing
./scripts/register.ps1
./scripts/register.ps1 -Config Release

# Verify registration
./scripts/verify.ps1

# Unregister
./scripts/unregister.ps1
```

## Development Workflow

### DLL Update Process
When modifying code, the DLL must be properly redeployed:

1. **Kill Explorer** to unlock the DLL: `taskkill //f //im explorer.exe`
2. **Copy new DLL** to registered location: `cp build/bin/Debug/AwesomeMenuHost.dll out/build/bin/Debug/AwesomeMenuHost.dll`
3. **Restart Explorer**: `powershell -Command "Start-Process explorer"`
4. **Test** by right-clicking in Windows Explorer

### Debug Output
The extension uses `OutputDebugStringW()` for diagnostics. Use DebugView or Visual Studio debug output to monitor execution.

## Architecture

### Core Innovation: IContextMenu3 Unlimited Menus

**The Problem**: Windows limits registry-based shell extensions to ~16 cascading items.

**The Solution**:
- Implement `IContextMenu3` interface
- Build menus programmatically in `buildCascadingMenuFixed()`
- Store relative command IDs to fix Windows' ID mapping bug
- Bypass registry parsing limitations entirely

### Key Components

- **AwesomeMenuHost** (`src/AwesomeMenuHost.cpp`): Main COM object, implements unlimited menu building
- **ClassFactory** (`src/ClassFactory.cpp`): COM factory with DLL reference counting
- **DLL Main** (`src/dllmain.cpp`): Standard COM exports, per-user registration (no elevation)
- **GUIDs** (`src/Guids.cpp`): COM class identifiers with single-definition pattern

### Data Structures

```cpp
struct FlyoutItem {
    std::wstring label;      // "Open Notepad"
    std::wstring command;    // "notepad.exe"
    std::wstring args;       // "%DIR%" (placeholder expansion)
    bool runAs;              // UAC elevation flag
    std::wstring icon;       // Icon extraction source
};

struct Flyout {
    std::vector<FlyoutItem> items;        // Direct commands
    std::vector<Flyout> subFlyouts;       // UNLIMITED nesting depth
};
```

### Hybrid Configuration System

**Always Present**: Core AwesomeMenu with 15+ Visual Studio admin development shells

**User-Extensible**: Registry files in `%APPDATA%\AwesomeMenuHost\menus\*.reg` become separate flyouts

**Hot-Reloadable**: Users add .reg files without recompilation - just right-click to reload

### Context-Aware Intelligence

Enhanced context detection beyond basic file/directory:
- **File Types**: .txt, .jpg, .exe, .zip etc. → different menu content
- **Drive Types**: Hard, removable, network → specialized tools
- **Locations**: Desktop, Documents, System folders → location-specific commands

### Critical Bug Fix: ID Mapping

**Issue**: Windows gives absolute IDs when building menus but relative IDs when executing commands.

**Solution**: Store relative IDs in mapping table:
```cpp
UINT relativeId = mi.wID - idCmdFirst;
m_idToPath[relativeId] = itemPath;  // Fixed execution bug
```

## Registry Integration

Shell extension registers as context menu handler for:
- Directory backgrounds (empty space right-clicks)
- Directory folders (folder icon right-clicks)
- All file types (file right-clicks)

Registration uses **HKEY_CURRENT_USER** for no-elevation installation.

## Testing

Right-click testing should show:
```
Context Menu:
├── Awesome Menu!!!              ← Always present (hardcoded)
├── my-tools                     ← From my-tools.reg
├── simple-test                  ← From simple-test.reg
└── windows-drive-tools          ← From windows-drive-tools.reg
```

Different contexts (text files, images, drives) should show intelligent menu adaptation.

## Technical Constraints

- **x64 only**: Modern Windows shell extensions require 64-bit
- **COM threading**: Uses Apartment threading model
- **Memory management**: All HBITMAP icons tracked for cleanup
- **Exception safety**: Never let C++ exceptions escape COM methods
- **Path limitations**: MAX_PATH constraints for Windows compatibility

## Performance Notes

- Configuration loads lazily (only when right-clicking)
- Icons extracted at runtime with caching
- Registry file parsing is simplified for reliability
- Emergency fallback ensures menu always appears

## Security Features

- Per-user registration (no elevation required)
- Input validation on registry parsing (32KB limits)
- Placeholder expansion with bounds checking
- UAC integration for elevated commands
- Path validation prevents directory traversal