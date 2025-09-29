/*
 * AwesomeMenuHost.h
 *
 * OVERVIEW:
 * =========
 * This header defines the core architecture for AwesomeMenuHost, a Windows Shell Extension
 * that provides unlimited cascading context menus by bypassing Windows' 16-item limit.
 *
 * The system uses IContextMenu3 interface to programmatically build menus instead of
 * relying on Windows registry parsing, which has built-in limitations.
 *
 * KEY INNOVATIONS:
 * - Registry File Loader: Data-driven menu configuration from .reg files
 * - Unlimited Cascading: No 16-item Windows limit via programmatic menu generation
 * - Icon Support: Menu items with application icons
 * - Working Directory Support: Commands execute in proper context directory
 * - Elevation Support: UAC integration for admin commands
 *
 * ARCHITECTURE:
 * =============
 * FlyoutItem -> Individual menu commands (e.g., "Open Notepad")
 * Flyout -> Menu containers with multiple items and nested submenus
 * AwesomeMenuHost -> Main COM object implementing Windows Shell Extension interfaces
 */

#pragma once

// Windows API headers for shell extension development
#include <windows.h>        // Core Windows types and functions
#include <shlobj.h>         // Shell object interfaces (IShellExtInit, IContextMenu3)
#include <shlwapi.h>        // Shell utility functions (PathRemoveFileSpec, etc.)

// STL containers for menu structure management
#include <string>           // std::wstring for Unicode string handling
#include <vector>           // Dynamic arrays for menu items and flyouts
#include <map>              // ID-to-path mapping for menu command resolution

/*
 * FlyoutItem Structure
 * ====================
 * Represents a single executable menu item (e.g., "Open Notepad", "Launch VSCode")
 *
 * This structure contains all the information needed to:
 * 1. Display the menu item with proper label and icon
 * 2. Execute the command when clicked
 * 3. Handle UAC elevation if required
 * 4. Group items into sections with separators
 */
struct FlyoutItem {
    std::wstring label;         // Display name shown in context menu (e.g., "Open Notepad")
    std::wstring command;       // Executable to run (e.g., "notepad.exe", "code-insiders.exe")
    std::wstring args;          // Command line arguments (e.g., "\"%DIR%\"" for VSCode)
    std::wstring icon;          // Icon source (executable path for icon extraction)
    std::wstring workingDir;    // Working directory for command execution (%DIR% = context folder)
    bool runAs = false;         // UAC elevation flag (true = run as administrator)
    std::wstring section;       // Optional grouping label for menu organization (creates separators)
};

/*
 * Flyout Structure
 * ================
 * Represents a menu container that can hold multiple items and nested submenus.
 * This enables unlimited cascading menu depth, bypassing Windows' 16-item limit.
 *
 * A flyout can contain:
 * - Direct menu items (FlyoutItem objects)
 * - Nested submenus (other Flyout objects)
 * - Mixed content (items + submenus in the same container)
 */
struct Flyout {
    std::wstring name;                      // Internal identifier (used for debugging/logging)
    std::wstring label;                     // Display name for submenu (e.g., "As Admin", "Development Tools")
    std::wstring showIn;                    // Context filter (background;directory;file;multi)
    std::vector<FlyoutItem> items;          // Direct executable menu items
    std::vector<Flyout> subFlyouts;         // Nested cascading submenus (unlimited depth)
};

/*
 * AwesomeMenuHost Class
 * =====================
 * Main COM object implementing Windows Shell Extension interfaces.
 *
 * This class inherits from:
 * - IShellExtInit: Provides initialization context (selected files, clicked folder)
 * - IContextMenu3: Advanced context menu interface that bypasses Windows limitations
 *
 * CRITICAL DESIGN DECISION: IContextMenu3 vs IContextMenu
 * ========================================================
 * We use IContextMenu3 instead of basic IContextMenu because:
 * - IContextMenu: Limited to ~16 cascading menu items (Windows enforced)
 * - IContextMenu3: Supports unlimited items via programmatic menu building
 * - IContextMenu3: Provides advanced features (tooltips, keyboard navigation, custom drawing)
 *
 * COM OBJECT LIFECYCLE:
 * - Windows creates instance when user right-clicks
 * - Initialize() called with context (selected files, folder)
 * - QueryContextMenu() called to build menu structure
 * - InvokeCommand() called when user clicks menu item
 * - Object destroyed when context menu closes
 */

/*
 * Context Detection Enumeration
 * ==============================
 * Describes what the user right-clicked on to enable context-aware menu customization.
 */
enum class ContextKind {
    Background,         // Right-clicked on empty space in folder
    Directory,          // Right-clicked on a single folder
    File,              // Right-clicked on a single file (generic)
    Multi,             // Right-clicked on multiple items (files/folders)

    // File Type Contexts
    TextFile,          // .txt, .log, .md, .ini, .cfg, etc.
    ImageFile,         // .jpg, .png, .gif, .bmp, .ico, etc.
    ExecutableFile,    // .exe, .msi, .bat, .cmd, .ps1, etc.
    ArchiveFile,       // .zip, .rar, .7z, .tar, .gz, etc.
    DocumentFile,      // .pdf, .doc, .docx, .xls, .xlsx, .ppt, etc.
    CodeFile,          // .cpp, .h, .cs, .js, .py, .java, etc.
    MediaFile,         // .mp3, .mp4, .avi, .mkv, .wav, etc.

    // Drive Type Contexts
    HardDrive,         // Fixed drives (C:, D:, etc.)
    RemovableDrive,    // USB drives, SD cards, etc.
    NetworkDrive,      // Mapped network drives
    OpticalDrive,      // CD/DVD/Blu-ray drives

    // Special Location Contexts
    DesktopLocation,   // Desktop folder
    DocumentsLocation, // Documents folder
    SystemLocation,    // Windows, System32, Program Files, etc.
    ProjectLocation    // Detected development project folders
};

class AwesomeMenuHost : public IShellExtInit, public IContextMenu3 {
public:
    // Constructor/Destructor with COM reference counting
    AwesomeMenuHost();                      // Initialize COM object, increment global DLL reference
    virtual ~AwesomeMenuHost();             // Cleanup resources, decrement global DLL reference

    /*
     * IUnknown Interface Implementation
     * =================================
     * Standard COM object methods for interface querying and reference counting.
     * These methods are required for all COM objects.
     */
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;        // Interface discovery
    IFACEMETHODIMP_(ULONG) AddRef() override;                               // Increment reference count
    IFACEMETHODIMP_(ULONG) Release() override;                              // Decrement reference count (auto-delete at 0)

    /*
     * IShellExtInit Interface Implementation
     * ======================================
     * Provides context information when Windows initializes the shell extension.
     * This tells us what the user clicked on and where they clicked.
     */
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidlFolder,     // Folder being viewed (for background clicks)
                             IDataObject* pdtobj,               // Selected files/folders (for item clicks)
                             HKEY hkeyProgID) override;         // File type registry key (usually unused)

    /*
     * IContextMenu3 Interface Implementation
     * ======================================
     * Advanced context menu interface that provides unlimited menu capabilities.
     * This is where the magic happens - we build unlimited cascading menus programmatically.
     */
    IFACEMETHODIMP QueryContextMenu(HMENU hMenu,                // Existing context menu to modify
                                   UINT indexMenu,              // Position to insert our items
                                   UINT idCmdFirst,             // First command ID we can use
                                   UINT idCmdLast,              // Last command ID we can use
                                   UINT uFlags) override;       // Menu context flags (background vs selection)

    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO pici) override;      // Execute menu command when clicked

    IFACEMETHODIMP GetCommandString(UINT_PTR idCmd,             // Get tooltip/help text for menu item
                                   UINT uType,                  // Type of string requested
                                   UINT* pReserved,             // Reserved parameter
                                   LPSTR pszName,               // Output buffer
                                   UINT cchMax) override;       // Buffer size

    // IContextMenu2/3 advanced message handling for custom drawing and keyboard navigation
    IFACEMETHODIMP HandleMenuMsg(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    IFACEMETHODIMP HandleMenuMsg2(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* plResult) override;

private:
    /*
     * COM Object State Management
     * ============================
     */
    LONG m_ref = 1;                             // COM reference count (auto-delete when reaches 0)

    /*
     * Context Information (Set during Initialize())
     * =============================================
     * This information tells us what the user right-clicked on and where.
     */
    std::wstring m_contextDir;                  // Working directory context:
                                               // - Background click: the folder being viewed
                                               // - File selection: parent folder of selected files
    std::vector<std::wstring> m_selection;      // Full paths of selected files/folders (empty for background)

    /*
     * Menu Structure Storage
     * ======================
     * The loaded menu configuration that will be displayed to the user.
     */
    std::vector<Flyout> m_flyouts;              // Complete menu structure loaded from registry files or hardcoded

    /*
     * Command ID Management (Critical for Menu Execution)
     * ===================================================
     * Windows assigns each menu item a unique ID. We must store the mapping
     * from menu ID back to the actual command to execute.
     *
     * CRITICAL BUG FIX: Windows gives us absolute IDs (e.g., 31060) when building menus,
     * but sends relative IDs (e.g., 0) when executing commands. We store relative IDs
     * to fix this mapping issue.
     */
    UINT m_idBase = 0;                          // Unused legacy field (kept for compatibility)
    UINT m_idCmdFirst = 0;                      // Base ID assigned by Windows (used for relative ID calculation)
    std::map<UINT, std::vector<UINT>> m_idToPath; // Maps menu command ID -> path to flyout/item
                                               // Path format: [flyoutIdx, itemIdx, isItem(0)/isSubflyout(1)]

    /*
     * Menu Configuration Loading System
     * =================================
     * Multiple strategies for loading menu configurations, in priority order:
     * 1. Registry files (.reg files in %APPDATA%\AwesomeMenuHost\menus\)
     * 2. Hardcoded AwesomeMenu structure
     * 3. Emergency fallback menu
     */
    void loadConfig();                          // Main entry point - orchestrates hybrid loading strategy
    void loadFromRegistryFiles();               // LEGACY: Load menus from .reg files (data-driven)
    void loadRegistryFilesAsSeparateFlyouts();   // NEW: Each .reg file becomes separate flyout
    void createCompleteAwesomeMenu();           // LEGACY: Static AwesomeMenu with unlimited "As Admin" submenu
    void createContextAwareAwesomeMenu(ContextKind kind); // NEW: Dynamic context-aware AwesomeMenu
    void createUnlimitedTestMenu();             // DEVELOPMENT: Test menu for validation

    /*
     * Registry File Parsing System
     * =============================
     * Converts Windows .reg files into our internal Flyout structure.
     * Supports standard Windows registry syntax for shell extensions.
     */
    void parseRegistryFile(const std::wstring& filePath, Flyout& targetFlyout);       // LEGACY: Complex parser
    void parseRegistryFileSimplified(const std::wstring& filePath, Flyout& targetFlyout); // NEW: Simplified parser
    std::wstring getMenusFolder() const;        // Get %APPDATA%\AwesomeMenuHost\menus\ path

    /*
     * Legacy Registry Parsing (Original Implementation)
     * =================================================
     * These methods parse existing Windows registry entries (HKCR\Directory\shell\AwesomeMenu).
     * Used as fallback when registry files are not available.
     */
    void loadAndConvertShellExtensions();       // Load from live Windows registry
    void parseAwesomeMenuStructure(HKEY hAwesomeMenu);           // Parse main AwesomeMenu key
    void parseSubCommands(const std::wstring& subCommands,       // Parse SubCommands value (cascading structure)
                         const std::wstring& basePath, Flyout& flyout);
    void parseDirectCommand(const std::wstring& command,         // Parse individual command entry
                           const std::wstring& basePath, Flyout& flyout);

    /*
     * Menu Execution System
     * =====================
     * Handles command execution when user clicks menu items.
     */
    bool expandPlaceholders(std::wstring& s) const;            // Replace %DIR%, %SEL% with actual paths
    HRESULT runItem(const FlyoutItem& it) const;               // Execute menu command with ShellExecuteEx

    /*
     * Visual Enhancement System
     * =========================
     * Provides icons and visual improvements for menu items.
     */
    HBITMAP hbitmapFromIconSpec(const std::wstring& spec, int sizePx);  // Extract icon from executable
    std::vector<HBITMAP> m_menuBitmaps;         // Cache of menu icons (cleaned up on menu close)

    /*
     * Menu Building System (The Core Innovation)
     * ===========================================
     * These methods build unlimited cascading menus programmatically,
     * bypassing Windows' 16-item registry parsing limitations.
     */
    UINT buildCascadingMenu(HMENU hMenu, const Flyout& flyout,      // Legacy menu builder
                           UINT& idNext, std::vector<UINT>& currentPath);
    UINT buildCascadingMenuFixed(HMENU hParentMenu,                 // Fixed menu builder with proper ID mapping
                                const Flyout& flyout, UINT& idNext,
                                std::vector<UINT>& currentPath, UINT idCmdFirst);

    /*
     * Command Resolution System
     * =========================
     * Converts menu command IDs back to executable commands.
     * This is where the "ID not found in map" bug was fixed.
     */
    const FlyoutItem* findItemByPath(const std::vector<UINT>& path) const;  // Navigate path to find command
};