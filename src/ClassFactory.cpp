/*
 * ClassFactory.cpp - COM Object Factory Implementation
 *
 * OVERVIEW:
 * =========
 * This file implements the COM class factory that Windows uses to create instances
 * of our shell extension objects. The class factory serves as the bridge between
 * Windows' COM system and our custom shell extension implementations.
 *
 * COM OBJECT CREATION FLOW:
 * =========================
 * 1. Windows calls DllGetClassObject() requesting a factory for a specific CLSID
 * 2. We return a ClassFactory instance configured for that CLSID
 * 3. Windows calls CreateInstance() on the factory to create actual objects
 * 4. Factory creates appropriate object type based on stored CLSID
 * 5. Returns the new object to Windows for use
 *
 * SUPPORTED OBJECT TYPES:
 * =======================
 * - CLSID_AwesomeMenu: Creates AwesomeMenuHost shell extension objects
 * - CLSID_FlyoutExplorerCommand: Creates ExplorerCommandRoot objects (Windows 11+)
 *
 * MEMORY MANAGEMENT:
 * ==================
 * The g_cDllRef global counter tracks active COM objects. The DLL cannot be
 * unloaded while this counter is non-zero, ensuring object lifetime safety.
 */

#include "ClassFactory.h"
#include "Guids.h"              // CLSID definitions
#include "AwesomeMenuHost.h"     // Main shell extension implementation
#include "ExplorerCommands.h"    // Windows 11 Explorer Command implementation
#include <new>                   // std::nothrow for safe memory allocation

// Global DLL reference counter for COM object lifetime management
// CRITICAL: This prevents DLL unloading while COM objects are active
long g_cDllRef = 0;

/*
 * IUnknown::QueryInterface Implementation for ClassFactory
 * ========================================================
 * Standard COM interface discovery for class factory objects.
 * Class factories only need to support IUnknown and IClassFactory interfaces.
 *
 * INTERFACE SUPPORT:
 * - IUnknown: Base COM interface (required for all COM objects)
 * - IClassFactory: Factory interface for creating other COM objects
 */
IFACEMETHODIMP ClassFactory::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;               // Validate output pointer
    *ppv = nullptr;                           // Initialize to null

    // Check for supported interfaces
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();                             // Increment reference count
        return S_OK;
    }

    return E_NOINTERFACE;                     // Interface not supported
}

/*
 * IClassFactory::CreateInstance Implementation
 * ============================================
 * Creates instances of our shell extension objects based on the CLSID
 * this factory was configured for during construction.
 *
 * OBJECT CREATION PROCESS:
 * 1. Validate parameters (no aggregation, valid output pointer)
 * 2. Determine object type from stored CLSID
 * 3. Allocate new object using safe allocation (std::nothrow)
 * 4. Query for requested interface on new object
 * 5. Release initial reference (QueryInterface adds its own)
 * 6. Return interface pointer to caller
 *
 * SUPPORTED OBJECT TYPES:
 * - CLSID_AwesomeMenu: Main shell extension with unlimited cascading menus
 * - CLSID_FlyoutExplorerCommand: Windows 11+ Explorer Command integration
 *
 * MEMORY SAFETY:
 * - Uses std::nothrow to handle allocation failures gracefully
 * - Proper reference counting with initial Release() call
 * - Returns appropriate error codes for all failure conditions
 */
IFACEMETHODIMP ClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) {
    // COM aggregation not supported
    if (pUnkOuter) return CLASS_E_NOAGGREGATION;

    // Validate output pointer
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    // Create object based on CLSID this factory was configured for
    if (m_clsid == CLSID_AwesomeMenu) {
        // Create main shell extension object
        AwesomeMenuHost* ext = new (std::nothrow) AwesomeMenuHost();
        if (!ext) return E_OUTOFMEMORY;       // Allocation failed

        // Query for requested interface
        HRESULT hr = ext->QueryInterface(riid, ppv);
        ext->Release();                       // Release initial reference
        return hr;
    }
    else if (m_clsid == CLSID_FlyoutExplorerCommand) {
        // Create Windows 11+ Explorer Command object
        ExplorerCommandRoot* cmd = new (std::nothrow) ExplorerCommandRoot();
        if (!cmd) return E_OUTOFMEMORY;       // Allocation failed

        // Query for requested interface
        HRESULT hr = cmd->QueryInterface(riid, ppv);
        cmd->Release();                       // Release initial reference
        return hr;
    }

    // Unknown CLSID - this factory wasn't configured for this object type
    return CLASS_E_CLASSNOTAVAILABLE;
}