/*
 * ClassFactory.h - COM Object Factory
 *
 * OVERVIEW:
 * =========
 * This header defines the COM class factory that Windows uses to create instances
 * of our AwesomeMenuHost shell extension. The class factory is the bridge between
 * Windows' COM system and our custom shell extension objects.
 *
 * COM ARCHITECTURE ROLE:
 * ======================
 * When Windows needs a shell extension:
 * 1. Windows calls DllGetClassObject() in our DLL
 * 2. We return a ClassFactory instance for the requested CLSID
 * 3. Windows calls CreateInstance() on the factory
 * 4. Factory creates and returns a new AwesomeMenuHost object
 * 5. Windows uses the AwesomeMenuHost for context menu operations
 *
 * WHY WE NEED THIS:
 * =================
 * COM requires a factory pattern - Windows doesn't directly instantiate our objects.
 * Instead, it asks for a factory that can create objects. This provides:
 * - Abstraction layer for object creation
 * - Reference counting for DLL lifetime management
 * - Support for multiple object types in one DLL
 */

#pragma once
#include <windows.h>        // Core Windows types
#include <unknwn.h>         // IUnknown interface definition

/*
 * ClassFactory Class
 * ==================
 * COM factory object that creates AwesomeMenuHost instances on demand.
 * Implements IClassFactory interface required by Windows COM system.
 *
 * LIFECYCLE:
 * - Created once per CLSID when Windows first requests our shell extension
 * - Kept alive as long as Windows needs to create objects
 * - Destroyed when Windows no longer needs our objects
 */
class ClassFactory : public IClassFactory {
public:
    /*
     * Constructor: Store the CLSID we're responsible for creating
     * Destructor: Clean up resources when factory is no longer needed
     */
    ClassFactory(REFCLSID clsid) : m_clsid(clsid) { InterlockedIncrement(&m_ref); }
    virtual ~ClassFactory() { InterlockedDecrement(&m_ref); }

    /*
     * IUnknown Interface Implementation
     * =================================
     * Standard COM object lifetime management.
     */
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;    // Interface discovery
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }    // Increment reference
    IFACEMETHODIMP_(ULONG) Release() override {                        // Decrement reference, auto-delete at 0
        ULONG c = InterlockedDecrement(&m_ref);
        if (!c) delete this;
        return c;
    }

    /*
     * IClassFactory Interface Implementation
     * ======================================
     * The core factory methods that Windows uses to create our objects.
     */
    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter,      // Outer object for aggregation (usually NULL)
                                 REFIID riid,               // Interface requested on new object
                                 void** ppv) override;      // Output: pointer to created object

    IFACEMETHODIMP LockServer(BOOL fLock) override {        // Keep DLL loaded in memory
        if (fLock) InterlockedIncrement(&m_ref);            // Lock: prevent DLL unload
        else InterlockedDecrement(&m_ref);                  // Unlock: allow DLL unload
        return S_OK;
    }

private:
    /*
     * Factory State
     * =============
     */
    LONG m_ref = 1;         // COM reference count (factory lifetime management)
    CLSID m_clsid;          // CLSID of objects this factory creates (CLSID_AwesomeMenu)
};