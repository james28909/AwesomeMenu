/*
 * Guids.h - COM Object GUID Declarations
 *
 * OVERVIEW:
 * =========
 * This header declares the globally unique identifiers (GUIDs) used by the
 * AwesomeMenuHost shell extension system. GUIDs are required for COM object
 * registration and interface identification in Windows.
 *
 * WHY SEPARATE GUID FILE:
 * =======================
 * Originally, GUIDs were defined inline in header files, but this caused
 * linker errors when multiple source files included the same GUID definitions.
 * By declaring them as 'extern' here and defining them in Guids.cpp, we
 * ensure single-definition rule compliance.
 *
 * GUID GENERATION:
 * ================
 * These GUIDs were generated using Visual Studio's "Create GUID" tool or
 * online GUID generators. They must be unique globally and should never
 * be changed once the extension is deployed.
 */

#pragma once
#include <windows.h>

/*
 * CLSID_AwesomeMenu
 * =================
 * Primary COM class identifier for the AwesomeMenuHost shell extension.
 * This GUID is used for:
 * - COM object registration in Windows registry
 * - Shell extension handler registration
 * - Windows Explorer loading the correct DLL when user right-clicks
 *
 * Registry Location: HKCU\Software\Classes\CLSID\{E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}
 * Handler Registration: HKCU\Software\Classes\Directory\shellex\ContextMenuHandlers\AwesomeMenuHost
 */
extern const GUID CLSID_AwesomeMenu;        // {E0E8C3B2-1E8C-4C15-9A4F-8A7C0F4A7F10}

/*
 * CLSID_FlyoutExplorerCommand
 * ============================
 * Secondary COM class identifier for Windows 11 Explorer Command integration.
 * This provides an alternative registration method for newer Windows versions
 * that support the IExplorerCommand interface alongside traditional IContextMenu.
 *
 * NOTE: This is currently unused but reserved for future Windows 11+ features.
 */
extern const GUID CLSID_FlyoutExplorerCommand;  // {9EFC1B28-0B9C-4D8D-A2C8-0F4B4F8A5C22}