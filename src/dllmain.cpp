/*
 * dllmain.cpp - Windows COM Shell Extension DLL Main Module
 *
 * OVERVIEW:
 * =========
 * This file implements the main entry points for the AwesomeMenuHost Windows Shell Extension DLL.
 * It provides the standard COM DLL exports required for Windows to load, register, and use
 * our shell extension in Windows Explorer.
 *
 * COM DLL EXPORTS:
 * ================
 * - DllMain: DLL initialization and cleanup
 * - DllGetClassObject: Creates COM class factories for our objects
 * - DllCanUnloadNow: Determines if DLL can be safely unloaded
 * - DllRegisterServer: Registers shell extension with Windows
 * - DllUnregisterServer: Removes shell extension registration
 *
 * SHELL EXTENSION REGISTRATION:
 * =============================
 * The DLL registers itself as a context menu handler for:
 * - Directory backgrounds (right-click on empty space in folders)
 * - Directory folders (right-click on folder icons)
 * - Uses per-user registration (HKEY_CURRENT_USER) to avoid requiring elevation
 *
 * This enables unlimited cascading context menus that bypass Windows' 16-item limit.
 */

#include <windows.h>        // Core Windows API definitions
#include "Guids.h"          // Contains CLSID definitions for our COM classes
#include "ClassFactory.h"   // COM class factory implementation
#include <new>              // For std::nothrow operator

// Reference to the DLL module base address - used to get the DLL file path
extern "C" IMAGE_DOS_HEADER __ImageBase;

// Global reference counter for tracking active COM objects in this DLL
// Used by DllCanUnloadNow() to determine if the DLL can be safely unloaded
extern long g_cDllRef;

/**
 * DLL Entry Point - Called when the DLL is loaded, unloaded, or when threads are created/destroyed
 * 
 * @param hModule Handle to the DLL module
 * @param ul_reason_for_call Reason for calling this function (DLL_PROCESS_ATTACH, DLL_PROCESS_DETACH, etc.)
 * @param LPVOID Reserved parameter (unused)
 * @return TRUE to indicate successful initialization, FALSE to abort loading
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    // Only handle process attachment - when the DLL is first loaded into a process
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        // Disable thread attach/detach notifications to improve performance
        // Since this shell extension doesn't need per-thread initialization
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE; // Always return TRUE to allow DLL loading to continue
}

/**
 * Standard COM Export - Creates instances of COM classes provided by this DLL
 * Called by COM system when a client requests an object of a specific CLSID
 * 
 * @param rclsid Reference to the CLSID (Class ID) of the requested COM class
 * @param riid Reference to the IID (Interface ID) of the requested interface
 * @param ppv Pointer to receive the created object interface
 * @return S_OK on success, appropriate error HRESULT on failure
 */
/*
 * Standard COM Export - Creates Class Factory Instances
 * =====================================================
 * This is THE CRITICAL ENTRY POINT that Windows uses to create our shell extension objects.
 * When users right-click in Explorer, Windows calls this function to get a factory
 * that can create AwesomeMenuHost instances.
 *
 * COM OBJECT CREATION FLOW:
 * 1. User right-clicks in Windows Explorer
 * 2. Windows looks up our CLSID in the registry
 * 3. Windows loads our DLL and calls DllGetClassObject
 * 4. We return a ClassFactory configured for the requested CLSID
 * 5. Windows calls CreateInstance on the factory
 * 6. Factory creates AwesomeMenuHost object with unlimited menu capability
 * 7. Windows calls Initialize and QueryContextMenu on our object
 * 8. Our unlimited cascading menus appear in the context menu
 */
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    // Validate output parameter pointer
    if (!ppv) return E_POINTER;

    // Initialize output parameter to null
    *ppv = nullptr;

    // Check if the requested CLSID matches one of our supported classes
    if (rclsid == CLSID_AwesomeMenu || rclsid == CLSID_FlyoutExplorerCommand) {
        // Create a class factory instance for the requested class
        // Use std::nothrow to avoid exceptions on allocation failure
        ClassFactory* fac = new (std::nothrow) ClassFactory(rclsid);
        if (!fac) return E_OUTOFMEMORY;

        // Query the class factory for the requested interface
        HRESULT hr = fac->QueryInterface(riid, ppv);

        // Release our reference to the class factory (client now owns it if successful)
        fac->Release();
        return hr;
    }

    // CLSID not supported by this DLL
    return CLASS_E_CLASSNOTAVAILABLE;
}

/**
 * Standard COM Export - Determines if the DLL can be safely unloaded
 * Called by COM system to check if all objects created by this DLL have been released
 * 
 * @return S_OK if DLL can be unloaded (no active objects), S_FALSE otherwise
 */
extern "C" HRESULT __stdcall DllCanUnloadNow(void) {
    // Return S_OK only if no COM objects from this DLL are currently active
    // g_cDllRef is incremented/decremented by objects as they are created/destroyed
    return (g_cDllRef == 0) ? S_OK : S_FALSE;
}

/**
 * Helper function to write string values to the Windows Registry
 * Creates registry keys as needed and sets string values for shell extension registration
 * 
 * @param root Root registry key (e.g., HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE)
 * @param subKey Path to the registry subkey to create/open
 * @param name Name of the registry value (nullptr for default value)
 * @param value String value to write to the registry
 * @return S_OK on success, appropriate error HRESULT on failure
 */
static HRESULT WriteRegSZ(HKEY root, const wchar_t* subKey, const wchar_t* name, const wchar_t* value) {
    // Create or open the registry key with write access
    HKEY h{}; LONG r = RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    
    // Write the string value to the registry key
    // Calculate size including null terminator for wide character string
    r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)value, (DWORD)((wcslen(value)+1)*sizeof(wchar_t)));
    
    // Always close the registry key handle
    RegCloseKey(h);
    
    // Convert Win32 error to HRESULT (0 maps to S_OK)
    return HRESULT_FROM_WIN32(r == ERROR_SUCCESS ? 0 : r);
}

/**
 * Standard COM Export - Registers this DLL's COM classes and shell extensions
 * Called during installation or when regsvr32 is used to register the DLL
 * Registers the context menu handler for directory backgrounds and folders
 * 
 * @return S_OK on successful registration, appropriate error HRESULT on failure
 */
extern "C" HRESULT __stdcall DllRegisterServer(void) {
    // Get the full path to this DLL module for registration
    wchar_t modulePath[MAX_PATH]; 
    GetModuleFileNameW((HMODULE)&__ImageBase, modulePath, ARRAYSIZE(modulePath));
    
    // Convert the CLSID to a string format for registry keys
    wchar_t clsidStr[64]; 
    StringFromGUID2(CLSID_AwesomeMenu, clsidStr, ARRAYSIZE(clsidStr));

    // Use per-user registration under HKCU\Software\Classes so elevation is not required
    // This allows installation without administrator privileges
    
    // Register the COM class as an in-process server (DLL)
    // Path: HKCU\Software\Classes\CLSID\{CLSID}\InprocServer32
    wchar_t inproc[512]; 
    wsprintfW(inproc, L"Software\\Classes\\CLSID\\%s\\InprocServer32", clsidStr);
    
    // Set the DLL path as the server location
    HRESULT hr = WriteRegSZ(HKEY_CURRENT_USER, inproc, nullptr, modulePath);
    if (FAILED(hr)) return hr;
    
    // Set the threading model to "Apartment" for shell extensions
    // This is required for proper COM marshaling in Explorer
    hr = WriteRegSZ(HKEY_CURRENT_USER, inproc, L"ThreadingModel", L"Apartment");
    if (FAILED(hr)) return hr;

    // Register the context menu handler for directory backgrounds
    // This makes the menu appear when right-clicking on empty space in folders
    hr = WriteRegSZ(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\AwesomeMenuHost", nullptr, clsidStr);
    if (FAILED(hr)) return hr;
    
    // Register the context menu handler for directory folders themselves
    // This makes the menu appear when right-clicking on folder icons
    hr = WriteRegSZ(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\AwesomeMenuHost", nullptr, clsidStr);
    if (FAILED(hr)) return hr;

    // NOTE: We don't create shell entries since AwesomeMenuHost reads from existing AwesomeMenu registry
    // The AwesomeMenu should appear directly without a "FlyoutHost" wrapper entry
    if (FAILED(hr)) return hr;
    return S_OK;
}

/**
 * Standard COM Export - Unregisters this DLL's COM classes and shell extensions
 * Called during uninstallation or when regsvr32 /u is used to unregister the DLL
 * Removes all registry entries created during registration
 * 
 * @return S_OK (always succeeds, even if some deletions fail)
 */
extern "C" HRESULT __stdcall DllUnregisterServer(void) {
    // Convert the CLSID to string format for registry key paths
    wchar_t clsidStr[64]; 
    StringFromGUID2(CLSID_AwesomeMenu, clsidStr, ARRAYSIZE(clsidStr));
    
    // Build the InprocServer32 registry key path
    wchar_t inproc[512]; 
    wsprintfW(inproc, L"Software\\Classes\\CLSID\\%s\\InprocServer32", clsidStr);
    
    // Delete the COM class registration (InprocServer32 key and all subkeys)
    RegDeleteTreeW(HKEY_CURRENT_USER, inproc);
    
    // Delete the directory background context menu handler registration
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\AwesomeMenuHost");
    
    // Delete the directory folder context menu handler registration
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shellex\\ContextMenuHandlers\\AwesomeMenuHost");
    
    // Commented out: Additional registration for all file types (not currently used)
    // RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\AwesomeMenuHost");
    
    // Always return success - registry cleanup errors are generally not critical
    return S_OK;
}