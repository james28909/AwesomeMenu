/*
 * Guids.cpp - COM Object GUID Definitions
 *
 * OVERVIEW:
 * =========
 * This file provides the actual definitions for the globally unique identifiers (GUIDs)
 * used by the AwesomeMenuHost shell extension system. These GUIDs are declared as
 * 'extern' in Guids.h and defined here to ensure single-definition rule compliance.
 *
 * SINGLE DEFINITION RULE:
 * =======================
 * Originally, GUIDs were defined inline in header files using DEFINE_GUID macros,
 * but this caused linker errors when multiple source files included the same headers.
 * By declaring GUIDs as 'extern const' in the header and defining them once here,
 * we ensure each GUID has exactly one definition in the entire program.
 *
 * GUID GENERATION:
 * ================
 * These GUIDs were generated using Visual Studio's "Create GUID" tool to ensure
 * global uniqueness. They must NEVER be changed once the extension is deployed,
 * as Windows uses these IDs to locate and load our shell extension.
 *
 * REGISTRY INTEGRATION:
 * =====================
 * These GUIDs appear in the Windows registry under:
 * - HKEY_CLASSES_ROOT\CLSID\{GUID} for COM object registration
 * - HKEY_CURRENT_USER\Software\Classes\Directory\shellex\ContextMenuHandlers\{GUID}
 * - Installation and uninstallation scripts use these exact GUID values
 */

#include "Guids.h"

/*
 * CLSID_AwesomeMenu Definition
 * ============================
 * Primary COM class identifier: {E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}
 *
 * This GUID identifies the AwesomeMenuHost shell extension to Windows.
 * When users right-click in Explorer, Windows uses this CLSID to:
 * 1. Locate our DLL in the registry
 * 2. Load the DLL into Explorer's process
 * 3. Create instances of our shell extension objects
 * 4. Display our unlimited cascading context menus
 */
const GUID CLSID_AwesomeMenu = {
    0xe0e8c3b2, 0x1e8c, 0x4c15,
    {0x9a, 0x4f, 0x8a, 0x7c, 0x0f, 0x4a, 0x7f, 0x10}
};

/*
 * CLSID_FlyoutExplorerCommand Definition
 * ======================================
 * Secondary COM class identifier: {9EFC1B28-0B9C-4D8D-A2C8-0F4B4F8A5C22}
 *
 * This GUID is reserved for Windows 11+ Explorer Command integration.
 * The IExplorerCommand interface provides an alternative registration
 * method for newer Windows versions alongside traditional IContextMenu.
 *
 * CURRENT STATUS: Reserved for future implementation
 * PLANNED USE: Windows 11 native command integration
 */
const GUID CLSID_FlyoutExplorerCommand = {
    0x9efc1b28, 0x0b9c, 0x4d8d,
    {0xa2, 0xc8, 0x0f, 0x4b, 0x4f, 0x8a, 0x5c, 0x22}
};