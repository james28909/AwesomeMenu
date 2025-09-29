/*
 * ExplorerCommands.cpp - Windows 11+ Explorer Command Implementation
 *
 * OVERVIEW:
 * =========
 * This file implements the IExplorerCommand interface for Windows 11+ integration.
 * While AwesomeMenuHost primarily uses IContextMenu3 for unlimited cascading menus,
 * this provides an alternative modern interface for newer Windows versions.
 *
 * IEXPLORERCOMMAND vs ICONTEXTMENU:
 * =================================
 * - IContextMenu3: Traditional shell extension interface (Windows 95+)
 *   - Supports unlimited cascading menus (our main implementation)
 *   - Complex but powerful, allows bypassing Windows' 16-item limit
 *   - Works on all Windows versions
 *
 * - IExplorerCommand: Modern command interface (Windows Vista+, improved in 11)
 *   - Simpler implementation model
 *   - Better integration with Windows 11 UI
 *   - Limited cascading capability (still subject to system limits)
 *
 * CURRENT STATUS:
 * ===============
 * This implementation is a PLACEHOLDER for future Windows 11+ features.
 * The main functionality is provided by AwesomeMenuHost (IContextMenu3).
 * This could be enhanced to provide Windows 11 native command integration.
 */

#include "ExplorerCommands.h"
#include <shlwapi.h>         // Shell utility functions (SHStrDupW)
#include <shellapi.h>        // Shell API functions

#ifndef ECF_ISSEPARATORIFEMPTY
#define ECF_ISSEPARATORIFEMPTY 0x00000004 // Windows 11+ flag, not in older SDKs
#endif
/*
 * IUnknown::QueryInterface Implementation for ExplorerCommand
 * ===========================================================
 * Standard COM interface discovery for Explorer Command objects.
 * This is much simpler than the full shell extension interface.
 */
IFACEMETHODIMP ExplorerCommandRoot::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;               // Validate output pointer
    *ppv = nullptr;                           // Initialize to null

    // Check for supported interfaces
    if (riid == IID_IUnknown || riid == IID_IExplorerCommand) {
        *ppv = static_cast<IExplorerCommand*>(this);
        AddRef();                             // Increment reference count
        return S_OK;
    }

    return E_NOINTERFACE;                     // Interface not supported
}

/*
 * IExplorerCommand Interface Implementation
 * =========================================
 * These methods provide the command information and behavior for Windows 11+ integration.
 * Currently a simplified placeholder implementation.
 */

// GetTitle: Returns the display name for the command
IFACEMETHODIMP ExplorerCommandRoot::GetTitle(IShellItemArray*, LPWSTR* ppszName) {
    return SHStrDupW(L"Awesome Menu", ppszName); // Allocates and returns command title
}

// GetIcon: Returns the icon for the command (not implemented)
IFACEMETHODIMP ExplorerCommandRoot::GetIcon(IShellItemArray*, LPWSTR* ppszIcon) {
    *ppszIcon = nullptr;                      // No custom icon specified
    return S_OK;
}

// GetToolTip: Returns tooltip text for the command
IFACEMETHODIMP ExplorerCommandRoot::GetToolTip(IShellItemArray*, LPWSTR* ppszInfotip) {
    return SHStrDupW(L"Awesome Menu Tools", ppszInfotip); // Allocates and returns tooltip
}

// GetCanonicalName: Returns unique identifier for the command
IFACEMETHODIMP ExplorerCommandRoot::GetCanonicalName(GUID* pguidCommandName) {
    *pguidCommandName = GUID_NULL;            // No specific canonical name
    return S_OK;
}

// GetState: Returns current state of the command (enabled, visible, etc.)
IFACEMETHODIMP ExplorerCommandRoot::GetState(IShellItemArray*, BOOL, EXPCMDSTATE* pCmdState) {
    *pCmdState = ECS_ENABLED;                 // Always enabled
    return S_OK;
}

// Invoke: Executes the command when clicked
IFACEMETHODIMP ExplorerCommandRoot::Invoke(IShellItemArray*, IBindCtx*) {
    // PLACEHOLDER: Simple message box implementation
    // TODO: Integrate with AwesomeMenuHost functionality for full menu system
    MessageBoxW(nullptr, L"AwesomeMenu invoked!", L"AwesomeMenuHost", MB_OK);
    return S_OK;
}

// GetFlags: Returns command behavior flags
IFACEMETHODIMP ExplorerCommandRoot::GetFlags(EXPCMDFLAGS* pFlags) {
    *pFlags = ECF_DEFAULT;                    // Default command behavior
    return S_OK;
}

// EnumSubCommands: Returns enumerator for subcommands (not implemented)
IFACEMETHODIMP ExplorerCommandRoot::EnumSubCommands(IEnumExplorerCommand** ppEnum) {
    *ppEnum = nullptr;                        // No subcommands implemented
    return E_NOTIMPL;                         // Future enhancement opportunity
}