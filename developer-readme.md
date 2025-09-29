# AwesomeMenuHost Developer Documentation

## Overview

AwesomeMenuHost is a Windows Shell Extension that provides **unlimited cascading context menus** by bypassing Windows' built-in 16-item limitation. The system uses the IContextMenu3 interface to build menus programmatically rather than relying on Windows' registry parsing, which has inherent restrictions.

## Architecture Overview

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Windows       │    │   AwesomeMenuHost │    │   Menu Config   │
│   Explorer      │◄──►│   Shell Extension │◄──►│   System        │
└─────────────────┘    └──────────────────┘    └─────────────────┘
         │                       │                       │
    User right-clicks       IContextMenu3           .reg files
    Context menu appears    Unlimited menus         Data-driven
```

## Key Innovation: Bypassing the 16-Item Limit

**The Problem**: Windows limits registry-based shell extensions to approximately 16 cascading menu items due to internal parsing restrictions.

**The Solution**: AwesomeMenuHost implements IContextMenu3 and builds menus programmatically in code, completely bypassing Windows' registry parsing limitations. This enables unlimited menu depth and item count.

## File Architecture

### Core Implementation Files

#### `src/AwesomeMenuHost.h` & `src/AwesomeMenuHost.cpp`
**Purpose**: Main shell extension implementation
- **Primary Role**: Core COM object implementing IShellExtInit + IContextMenu3
- **Key Innovation**: `buildCascadingMenuFixed()` method that creates unlimited menus
- **Data Structures**:
  - `FlyoutItem`: Individual executable menu commands
  - `Flyout`: Menu containers with unlimited nesting capability
- **Critical Features**:
  - Registry file loader system (data-driven configuration)
  - Placeholder expansion (%DIR%, %SEL%)
  - UAC elevation support
  - Icon extraction and caching
  - Context-aware menu filtering

#### `src/ClassFactory.h` & `src/ClassFactory.cpp`
**Purpose**: COM object factory
- **Role**: Bridge between Windows COM system and our objects
- **Function**: Creates AwesomeMenuHost instances on demand
- **COM Flow**: Windows → DllGetClassObject → ClassFactory → CreateInstance → AwesomeMenuHost
- **Lifetime Management**: Reference counting for proper object cleanup

#### `src/Guids.h` & `src/Guids.cpp`
**Purpose**: COM GUID definitions
- **Critical**: Contains the unique identifiers Windows uses to locate our extension
- **Primary CLSID**: `CLSID_AwesomeMenu` - {E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}
- **Secondary CLSID**: `CLSID_FlyoutExplorerCommand` - Future Windows 11+ integration
- **Single Definition Rule**: Prevents linker errors with multiple includes

#### `src/dllmain.cpp`
**Purpose**: DLL entry points and registration
- **DLL Exports**: DllMain, DllGetClassObject, DllCanUnloadNow, DllRegisterServer, DllUnregisterServer
- **Registration**: Creates Windows registry entries for shell extension activation
- **Scope**: Uses HKEY_CURRENT_USER for per-user registration (no elevation required)

#### `src/ExplorerCommands.h` & `src/ExplorerCommands.cpp`
**Purpose**: Windows 11+ Explorer Command integration (placeholder)
- **Interface**: IExplorerCommand (modern alternative to IContextMenu)
- **Status**: Reserved for future Windows 11 native integration
- **Current**: Simple placeholder implementation

## Data-Driven Menu System

### Registry File Loader
**Location**: `%APPDATA%\AwesomeMenuHost\menus\*.reg`

**Purpose**: Enables users to customize menus without recompiling

**Loading Priority**:
1. **Registry Files** (Primary): Load .reg files from menus folder
2. **Hardcoded Menu** (Fallback): Complete Visual Studio development environment
3. **Emergency Fallback** (Failsafe): Simple Windows Terminal admin command

**Registry File Format**:
```ini
[HKEY_CLASSES_ROOT\Directory\shell\MenuName]
"MUIVerb"="Display Name"
"Icon"="executable.exe"
"HasLUAShield"=dword:00000001

[HKEY_CLASSES_ROOT\Directory\shell\MenuName\command]
@="command to execute"
```

## Technical Implementation Details

### Menu Building Process

1. **Context Determination**: Analyze what user right-clicked (background, directory, file, multi-selection)
2. **Configuration Loading**: Load menu structure from registry files or hardcoded fallbacks
3. **Menu Filtering**: Apply showIn rules to display appropriate menus for context
4. **Programmatic Building**: Use IContextMenu3 to build unlimited cascading structure
5. **ID Mapping**: Store command ID → menu path mappings for execution
6. **Icon Enhancement**: Extract and cache icons from executables

### Command Execution Flow

1. **User Click**: User clicks menu item, Windows sends command ID
2. **ID Resolution**: Look up command ID in stored mapping table
3. **Path Navigation**: Navigate through flyout structure using stored path
4. **Placeholder Expansion**: Replace %DIR%, %SEL% with actual values
5. **Command Execution**: Use ShellExecuteEx with proper UAC elevation

### Critical Bug Fix: ID Mapping

**The Problem**: Windows gives absolute IDs when building menus but sends relative IDs when executing.

**The Solution**: Store relative IDs (id - idCmdFirst) in mapping table to ensure proper command resolution.

## Menu Structure

### FlyoutItem Structure
```cpp
struct FlyoutItem {
    std::wstring label;         // Display name (e.g., "Open Notepad")
    std::wstring command;       // Executable (e.g., "notepad.exe")
    std::wstring args;          // Arguments (e.g., ""%DIR%"")
    std::wstring icon;          // Icon source (executable path)
    std::wstring workingDir;    // Working directory (%DIR% = context folder)
    bool runAs = false;         // UAC elevation flag
    std::wstring section;       // Grouping for separators
};
```

### Flyout Structure
```cpp
struct Flyout {
    std::wstring name;                      // Internal identifier
    std::wstring label;                     // Display name for submenu
    std::wstring showIn;                    // Context filter (background;directory;file;multi)
    std::vector<FlyoutItem> items;          // Direct executable items
    std::vector<Flyout> subFlyouts;         // Nested submenus (unlimited depth!)
};
```

## COM Object Lifecycle

### Object Creation
1. User right-clicks in Explorer
2. Windows looks up CLSID_AwesomeMenu in registry
3. Windows loads DLL and calls DllGetClassObject
4. ClassFactory creates AwesomeMenuHost instance
5. Windows calls Initialize with context information
6. Windows calls QueryContextMenu to build menu

### Object Destruction
1. User closes context menu or selects item
2. Windows releases all interface references
3. AwesomeMenuHost reference count reaches zero
4. Object self-destructs, decrements global DLL reference
5. When all objects destroyed, DLL can be unloaded

## Development Workflow

### Building the Extension
1. Use Visual Studio with Windows SDK
2. Build as 64-bit DLL for modern Windows
3. Output: `AwesomeMenuHost.dll`

### Registration Process
```bash
# Register the shell extension
regsvr32 AwesomeMenuHost.dll

# Unregister the shell extension
regsvr32 /u AwesomeMenuHost.dll
```

### Testing and Debugging
1. **Registry Verification**: Check HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost
2. **Menu Testing**: Right-click in Explorer to verify menu appears
3. **Command Testing**: Click menu items to verify execution
4. **Debug Output**: Uses OutputDebugString for diagnostic messages

### Adding Custom Menus
1. Create .reg file in `%APPDATA%\AwesomeMenuHost\menus\`
2. Use standard Windows registry export format
3. Restart Explorer or re-register extension to reload
4. Verify menu appears in context menu

## Security Considerations

- **Input Validation**: All string operations include bounds checking
- **Memory Safety**: Uses smart pointers and RAII patterns
- **UAC Integration**: Proper elevation handling for admin commands
- **Path Safety**: Validates file paths and prevents directory traversal
- **COM Security**: Never lets C++ exceptions escape COM interface methods

## Extension Points

### Future Enhancements
1. **Enhanced Registry Parser**: Support more registry value types
2. **Dynamic Reloading**: File system watcher for hot-reload of .reg files
3. **Windows 11 Integration**: Full IExplorerCommand implementation
4. **Icon Caching**: Persistent icon cache for better performance
5. **Localization**: Multi-language support for menu labels
6. **Template System**: Menu templates for common development scenarios

## Troubleshooting

### Common Issues
1. **Menu Not Appearing**: Check registry entries and DLL registration
2. **Commands Not Executing**: Verify ID mapping and path resolution
3. **Memory Leaks**: Ensure proper bitmap cleanup in HandleMenuMsg2
4. **Access Violations**: Validate all pointer parameters in COM methods

### Debug Techniques
1. **DebugView**: Use Sysinternals DebugView to see OutputDebugString messages
2. **Process Monitor**: Monitor registry access and file operations
3. **Explorer Restart**: Kill and restart explorer.exe to reload shell extensions
4. **Event Viewer**: Check Windows logs for COM activation errors

This architecture enables unlimited cascading context menus while maintaining compatibility, performance, and extensibility for future enhancements.