/*
 * AwesomeMenuHost.cpp - Implementation of Unlimited Cascading Context Menu Shell Extension
 *
 * OVERVIEW:
 * =========
 * This file implements the core functionality of AwesomeMenuHost, a Windows Shell Extension
 * that bypasses Windows' 16-item cascading menu limitation by using IContextMenu3 interface
 * to build unlimited cascading menus programmatically.
 *
 * KEY INNOVATIONS:
 * ================
 * 1. REGISTRY FILE LOADER: Data-driven menu configuration from .reg files
 * 2. UNLIMITED CASCADING: No Windows 16-item limit via programmatic menu generation
 * 3. ICON SUPPORT: Automatic icon extraction from executables
 * 4. UAC ELEVATION: Built-in support for "Run as Administrator" commands
 * 5. CONTEXT AWARENESS: Different menus for background/directory/file/multi-selection
 *
 * MENU LOADING PRIORITY:
 * ======================
 * 1. Registry Files (.reg files in %APPDATA%\AwesomeMenuHost\menus\)
 * 2. Hardcoded AwesomeMenu structure (complete Visual Studio development environment)
 * 3. Emergency fallback menu (simple Windows Terminal admin command)
 *
 * TECHNICAL ARCHITECTURE:
 * =======================
 * - COM Object Model: Implements IShellExtInit + IContextMenu3 for Windows integration
 * - Menu ID Mapping: Critical bug fix for command execution (relative vs absolute IDs)
 * - Icon Caching: HBITMAP objects managed for menu visual enhancement
 * - Path Resolution: %DIR% and %SEL% placeholder expansion for dynamic commands
 */

#include "AwesomeMenuHost.h"
#include <shellapi.h>        // ShellExecuteEx for command execution
#include <vector>            // Dynamic arrays for menu structures
#include <map>               // ID-to-path mapping for command resolution
#include <memory>            // Smart pointers for resource management
#include <set>               // Tracking applied registry files
#include <shlwapi.h>         // Shell path utilities (PathRemoveFileSpec, PathIsDirectory)
#include <fstream>           // File I/O for registry file parsing
#include <shlobj.h>          // Shell folder APIs (SHGetKnownFolderPath)
#include <algorithm>         // std::transform for string manipulation
#include <format>            // std::format for structured logging
#include <cwctype>           // Character casing helpers
#include <set>               // Track managed registry files
#include <sstream>           // String composition helpers
#include <cstring>           // memcpy for registry value buffers
#include <cstdlib>           // wcstoul helpers

// Global DLL reference counter for COM object lifetime management
// Incremented when objects created, decremented when destroyed
// DLL cannot be unloaded while g_cDllRef > 0
extern long g_cDllRef;

std::wstring_view AwesomeMenuHost::logCategoryName(LogCategory category) noexcept {
    switch (category) {
        case LogCategory::Context: return L"Context";
        case LogCategory::Registry: return L"Registry";
        case LogCategory::Menu: return L"Menu";
        case LogCategory::Error: return L"Error";
        default: return L"General";
    }
}

void AwesomeMenuHost::logDebug(std::wstring_view message, LogCategory category) const {
    std::wstring composed;
    composed.reserve(message.size() + 32);
    composed.assign(L"AwesomeMenuHost");

    if (category != LogCategory::General) {
        composed.append(L"[");
        composed.append(logCategoryName(category));
        composed.append(L"]");
    }

    composed.append(L": ");
    composed.append(message);

    if (composed.empty() || composed.back() != L'\n') {
        composed.push_back(L'\n');
    }

    OutputDebugStringW(composed.c_str());
}

namespace {

constexpr const wchar_t* kManagedRegistryRoot = L"Software\\AwesomeMenuHost\\RegistryFiles";

void trimInPlace(std::wstring& s) {
    const auto first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        s.clear();
        return;
    }
    const auto last = s.find_last_not_of(L" \t\r\n");
    s.erase(last + 1);
    s.erase(0, first);
}

std::wstring toUpperCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towupper(ch));
    });
    return s;
}

bool startsWithInsensitive(const std::wstring& text, const wchar_t* prefix) {
    size_t len = wcslen(prefix);
    if (text.size() < len) return false;
    for (size_t i = 0; i < len; ++i) {
        if (towupper(text[i]) != towupper(prefix[i])) return false;
    }
    return true;
}

int hexDigit(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return static_cast<int>(ch - L'0');
    if (ch >= L'a' && ch <= L'f') return 10 + static_cast<int>(ch - L'a');
    if (ch >= L'A' && ch <= L'F') return 10 + static_cast<int>(ch - L'A');
    return -1;
}

std::wstring unescapeRegString(const std::wstring& input) {
    std::wstring result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        wchar_t ch = input[i];
        if (ch == L'\\' && i + 1 < input.size()) {
            wchar_t next = input[++i];
            switch (next) {
                case L'\\': result.push_back(L'\\'); break;
                case L'"': result.push_back(L'"'); break;
                case L'0': result.push_back(L'\0'); break;
                case L'n': result.push_back(L'\n'); break;
                case L'r': result.push_back(L'\r'); break;
                case L't': result.push_back(L'\t'); break;
                default:    result.push_back(next); break;
            }
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

std::vector<BYTE> parseHexBytes(const std::wstring& data) {
    std::vector<BYTE> bytes;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t start = pos;
        while (pos < data.size() && data[pos] != L',') {
            ++pos;
        }

        std::wstring token = data.substr(start, pos - start);
        trimInPlace(token);
        if (!token.empty()) {
            wchar_t* endPtr = nullptr;
            unsigned long value = wcstoul(token.c_str(), &endPtr, 16);
            if (endPtr != token.c_str()) {
                bytes.push_back(static_cast<BYTE>(value & 0xFF));
            }
        }

        if (pos < data.size() && data[pos] == L',') {
            ++pos;
        }
    }
    return bytes;
}

struct ParsedRegValue {
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
};

void ensureWideNullTerminator(std::vector<BYTE>& buffer) {
    if (buffer.size() % sizeof(wchar_t) != 0) {
        buffer.push_back(0);
    }
    if (buffer.size() % sizeof(wchar_t) != 0) {
        buffer.push_back(0);
    }

    const wchar_t* chars = reinterpret_cast<const wchar_t*>(buffer.data());
    size_t length = buffer.size() / sizeof(wchar_t);
    if (length == 0 || chars[length - 1] != L'\0') {
        buffer.push_back(0);
        buffer.push_back(0);
    }
}

std::vector<std::wstring> multiSzToVector(const wchar_t* data, size_t charCount) {
    std::vector<std::wstring> entries;
    size_t index = 0;
    while (index < charCount) {
        std::wstring entry = data + index;
        if (entry.empty()) {
            break;
        }
        entries.push_back(std::move(entry));
        index += entries.back().size() + 1;
    }
    return entries;
}

std::vector<wchar_t> vectorToMultiSz(const std::vector<std::wstring>& entries) {
    std::vector<wchar_t> buffer;
    for (const auto& entry : entries) {
        buffer.insert(buffer.end(), entry.begin(), entry.end());
        buffer.push_back(L'\0');
    }
    buffer.push_back(L'\0');
    return buffer;
}

bool parseRegistryValueData(const std::wstring& raw, ParsedRegValue& out) {
    std::wstring value = raw;
    trimInPlace(value);
    if (value.empty()) return false;

    if (value == L"-") {
        out.type = REG_NONE;
        out.data.clear();
        return true;
    }

    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        std::wstring inner = value.substr(1, value.size() - 2);
        std::wstring unescaped = unescapeRegString(inner);
        out.type = REG_SZ;
        out.data.resize((unescaped.size() + 1) * sizeof(wchar_t));
        memcpy(out.data.data(), unescaped.c_str(), (unescaped.size() + 1) * sizeof(wchar_t));
        return true;
    }

    if (startsWithInsensitive(value, L"dword:")) {
        std::wstring number = value.substr(6);
        trimInPlace(number);
        unsigned long parsed = wcstoul(number.c_str(), nullptr, 16);
        out.type = REG_DWORD;
        out.data.resize(sizeof(DWORD));
        memcpy(out.data.data(), &parsed, sizeof(DWORD));
        return true;
    }

    if (startsWithInsensitive(value, L"qword:")) {
        std::wstring number = value.substr(6);
        trimInPlace(number);
        unsigned long long parsed = _wcstoui64(number.c_str(), nullptr, 16);
        out.type = REG_QWORD;
        out.data.resize(sizeof(unsigned long long));
        memcpy(out.data.data(), &parsed, sizeof(unsigned long long));
        return true;
    }

    if (startsWithInsensitive(value, L"hex")) {
        size_t colonPos = value.find(L':');
        if (colonPos == std::wstring::npos) return false;

        std::wstring typeSpec = value.substr(0, colonPos);
        std::wstring dataSpec = value.substr(colonPos + 1);
        trimInPlace(dataSpec);

        DWORD regType = REG_BINARY;
        if (typeSpec.size() > 3) {
            size_t open = typeSpec.find(L'(');
            size_t close = typeSpec.find(L')');
            if (open != std::wstring::npos && close != std::wstring::npos && close > open + 1) {
                unsigned long typeCode = wcstoul(typeSpec.substr(open + 1, close - open - 1).c_str(), nullptr, 16);
                switch (typeCode) {
                    case 2: regType = REG_EXPAND_SZ; break;
                    case 7: regType = REG_MULTI_SZ; break;
                    default: regType = REG_BINARY; break;
                }
            }
        }

        std::vector<BYTE> bytes = parseHexBytes(dataSpec);
        if (regType == REG_EXPAND_SZ || regType == REG_MULTI_SZ) {
            ensureWideNullTerminator(bytes);
        }

        out.type = regType;
        out.data = std::move(bytes);
        return true;
    }

    // Fallback: treat as plain string without quotes
    std::wstring unescaped = unescapeRegString(value);
    out.type = REG_SZ;
    out.data.resize((unescaped.size() + 1) * sizeof(wchar_t));
    memcpy(out.data.data(), unescaped.c_str(), (unescaped.size() + 1) * sizeof(wchar_t));
    return true;
}

bool splitRegistryPath(const std::wstring& fullPath, HKEY& root, std::wstring& subKey, std::wstring& normalizedRoot) {
    if (fullPath.empty()) return false;

    size_t delim = fullPath.find(L'\\');
    std::wstring rootPart = (delim == std::wstring::npos) ? fullPath : fullPath.substr(0, delim);
    normalizedRoot = toUpperCopy(rootPart);

    if (normalizedRoot == L"HKEY_CURRENT_USER" || normalizedRoot == L"HKCU") {
        root = HKEY_CURRENT_USER;
    } else if (normalizedRoot == L"HKEY_CLASSES_ROOT" || normalizedRoot == L"HKCR") {
        root = HKEY_CLASSES_ROOT;
    } else if (normalizedRoot == L"HKEY_LOCAL_MACHINE" || normalizedRoot == L"HKLM") {
        root = HKEY_LOCAL_MACHINE;
    } else if (normalizedRoot == L"HKEY_USERS" || normalizedRoot == L"HKU") {
        root = HKEY_USERS;
    } else if (normalizedRoot == L"HKEY_CURRENT_CONFIG" || normalizedRoot == L"HKCC") {
        root = HKEY_CURRENT_CONFIG;
    } else {
        return false;
    }

    if (delim == std::wstring::npos) {
        subKey.clear();
    } else {
        subKey = fullPath.substr(delim + 1);
    }

    return true;
}

bool cleanupEmptyKey(HKEY root, const std::wstring& subKey) {
    if (subKey.empty()) return false;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD subKeyCount = 0;
    DWORD valueCount = 0;
    LONG queryResult = RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, &subKeyCount, nullptr, nullptr, &valueCount, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(hKey);
    if (queryResult != ERROR_SUCCESS) return false;

    if (subKeyCount == 0 && valueCount == 0) {
        size_t lastSlash = subKey.find_last_of(L'\\');
        std::wstring parent = (lastSlash == std::wstring::npos) ? std::wstring() : subKey.substr(0, lastSlash);
        std::wstring leaf = (lastSlash == std::wstring::npos) ? subKey : subKey.substr(lastSlash + 1);
        HKEY hParent = nullptr;
        if (RegOpenKeyExW(root, parent.empty() ? nullptr : parent.c_str(), 0, KEY_WRITE, &hParent) == ERROR_SUCCESS) {
            RegDeleteKeyW(hParent, leaf.c_str());
            RegCloseKey(hParent);
        }
        return true;
    }
    return false;
}

bool deleteRegistryValue(HKEY root, const std::wstring& subKey, const std::wstring& valueName) {
    HKEY hKey = nullptr;
    LONG status = RegOpenKeyExW(root, subKey.empty() ? nullptr : subKey.c_str(), 0,
                                KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &hKey);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    status = RegDeleteValueW(hKey, valueName.empty() ? nullptr : valueName.c_str());
    RegCloseKey(hKey);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    cleanupEmptyKey(root, subKey);
    return true;
}

} // namespace

/*
 * Enhanced Context Detection System
 * =================================
 * Determines which menu items to show based on what the user right-clicked.
 * This enables intelligent, context-sensitive menus that adapt to the current selection.
 *
 * CONTEXT TYPES:
 * - Basic: Background, Directory, File, Multi-selection
 * - File Types: Text files, Images, Executables, Archives, etc.
 * - Drive Types: Hard drives, USB drives, Network drives, CD/DVD
 * - Special Locations: Desktop, Documents, System folders, etc.
 */

/*
 * Enhanced Context Detection Helper Functions
 * ============================================
 * These functions analyze file extensions, drive types, and locations
 * to provide intelligent context-aware menu filtering.
 */

// File extension to context mapping
static ContextKind getFileTypeContext(const std::wstring& filePath) {
    if (filePath.empty()) return ContextKind::File;

    // Find the last dot for extension
    size_t dotPos = filePath.find_last_of(L'.');
    if (dotPos == std::wstring::npos || dotPos == filePath.length() - 1) {
        return ContextKind::File; // No extension or ends with dot
    }

    // Extract extension and convert to lowercase
    std::wstring ext = filePath.substr(dotPos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    // Text files
    if (ext == L"txt" || ext == L"log" || ext == L"md" || ext == L"ini" ||
        ext == L"cfg" || ext == L"conf" || ext == L"xml" || ext == L"json" ||
        ext == L"yaml" || ext == L"yml" || ext == L"csv" || ext == L"rtf") {
        return ContextKind::TextFile;
    }

    // Image files
    if (ext == L"jpg" || ext == L"jpeg" || ext == L"png" || ext == L"gif" ||
        ext == L"bmp" || ext == L"ico" || ext == L"tiff" || ext == L"webp" ||
        ext == L"svg" || ext == L"psd" || ext == L"raw") {
        return ContextKind::ImageFile;
    }

    // Executable files
    if (ext == L"exe" || ext == L"msi" || ext == L"bat" || ext == L"cmd" ||
        ext == L"ps1" || ext == L"vbs" || ext == L"com" || ext == L"scr" ||
        ext == L"app" || ext == L"dmg") {
        return ContextKind::ExecutableFile;
    }

    // Archive files
    if (ext == L"zip" || ext == L"rar" || ext == L"7z" || ext == L"tar" ||
        ext == L"gz" || ext == L"bz2" || ext == L"xz" || ext == L"cab" ||
        ext == L"iso" || ext == L"dmg") {
        return ContextKind::ArchiveFile;
    }

    // Document files
    if (ext == L"pdf" || ext == L"doc" || ext == L"docx" || ext == L"xls" ||
        ext == L"xlsx" || ext == L"ppt" || ext == L"pptx" || ext == L"odt" ||
        ext == L"ods" || ext == L"odp") {
        return ContextKind::DocumentFile;
    }

    // Code files
    if (ext == L"cpp" || ext == L"h" || ext == L"hpp" || ext == L"c" ||
        ext == L"cs" || ext == L"js" || ext == L"ts" || ext == L"py" ||
        ext == L"java" || ext == L"php" || ext == L"rb" || ext == L"go" ||
        ext == L"rs" || ext == L"swift" || ext == L"kt" || ext == L"scala" ||
        ext == L"html" || ext == L"css" || ext == L"scss" || ext == L"sass") {
        return ContextKind::CodeFile;
    }

    // Media files
    if (ext == L"mp3" || ext == L"mp4" || ext == L"avi" || ext == L"mkv" ||
        ext == L"wav" || ext == L"flac" || ext == L"ogg" || ext == L"mov" ||
        ext == L"wmv" || ext == L"webm" || ext == L"m4a" || ext == L"aac") {
        return ContextKind::MediaFile;
    }

    return ContextKind::File; // Default for unrecognized extensions
}

// Drive type detection
static ContextKind getDriveTypeContext(const std::wstring& drivePath) {
    if (drivePath.length() < 2 || drivePath[1] != L':') {
        return ContextKind::Background; // Not a drive path
    }

    wchar_t driveLetter[4] = { drivePath[0], L':', L'\\', L'\0' };
    UINT driveType = GetDriveTypeW(driveLetter);

    switch (driveType) {
        case DRIVE_FIXED:
            return ContextKind::HardDrive;
        case DRIVE_REMOVABLE:
            return ContextKind::RemovableDrive;
        case DRIVE_REMOTE:
            return ContextKind::NetworkDrive;
        case DRIVE_CDROM:
            return ContextKind::OpticalDrive;
        default:
            return ContextKind::Background;
    }
}

// Special location detection
static ContextKind getLocationContext(const std::wstring& folderPath) {
    if (folderPath.empty()) return ContextKind::Background;

    // Convert to lowercase for comparison
    std::wstring path = folderPath;
    std::transform(path.begin(), path.end(), path.begin(), ::towlower);

    // Desktop detection
    if (path.find(L"\\desktop") != std::wstring::npos) {
        return ContextKind::DesktopLocation;
    }

    // Documents detection
    if (path.find(L"\\documents") != std::wstring::npos ||
        path.find(L"\\my documents") != std::wstring::npos) {
        return ContextKind::DocumentsLocation;
    }

    // System folder detection
    if (path.find(L"\\windows") != std::wstring::npos ||
        path.find(L"\\system32") != std::wstring::npos ||
        path.find(L"\\program files") != std::wstring::npos ||
        path.find(L"\\programdata") != std::wstring::npos) {
        return ContextKind::SystemLocation;
    }

    // Project folder detection (look for common dev files)
    if (path.find(L".git") != std::wstring::npos ||
        path.find(L"package.json") != std::wstring::npos ||
        path.find(L"CMakeLists.txt") != std::wstring::npos ||
        path.find(L".sln") != std::wstring::npos ||
        path.find(L"pom.xml") != std::wstring::npos ||
        path.find(L"requirements.txt") != std::wstring::npos) {
        return ContextKind::ProjectLocation;
    }

    return ContextKind::Background;
}

/*
 * COM Object Lifecycle Management
 * ===============================
 * Constructor: Increment global DLL reference to prevent unloading while objects exist
 * Destructor: Decrement reference, allowing DLL unload when no objects remain
 *
 * CRITICAL: These reference counts determine when Windows can safely unload our DLL
 */
AwesomeMenuHost::AwesomeMenuHost() {
    InterlockedIncrement(&g_cDllRef);     // Thread-safe increment for COM apartment threading
}

AwesomeMenuHost::~AwesomeMenuHost() {
    InterlockedDecrement(&g_cDllRef);     // Thread-safe decrement, enables DLL unloading
}

/*
 * IUnknown::QueryInterface Implementation
 * =======================================
 * Standard COM interface discovery mechanism. Windows calls this to check
 * if our object supports specific interfaces.
 *
 * SUPPORTED INTERFACES:
 * - IUnknown: Base COM interface (all COM objects must support)
 * - IContextMenu/IContextMenu2/IContextMenu3: Context menu interfaces (we use IContextMenu3)
 * - IShellExtInit: Shell extension initialization interface
 *
 * CRITICAL: We cast to IContextMenu3 for all context menu interfaces because
 * IContextMenu3 inherits from IContextMenu2 which inherits from IContextMenu.
 * This ensures consistent vtable layout for all context menu calls.
 */
IFACEMETHODIMP AwesomeMenuHost::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;           // Null output pointer check
    *ppv = nullptr;                       // Initialize output to null

    // Check for context menu interfaces (all versions)
    if (riid == IID_IUnknown || riid == IID_IContextMenu || riid == IID_IContextMenu2 || riid == IID_IContextMenu3) {
        *ppv = static_cast<IContextMenu3*>(this);     // Always cast to highest version (IContextMenu3)
    }
    // Check for shell extension initialization interface
    else if (riid == IID_IShellExtInit) {
        *ppv = static_cast<IShellExtInit*>(this);
    }
    // Interface not supported
    else {
        return E_NOINTERFACE;
    }

    AddRef();                            // Increment reference count for returned interface
    return S_OK;
}

/*
 * IUnknown Reference Counting Implementation
 * ==========================================
 * Standard COM object lifetime management through reference counting.
 * These methods are called by Windows and other COM clients to manage object lifetime.
 */

// AddRef: Increment reference count when interface pointer is copied or stored
IFACEMETHODIMP_(ULONG) AwesomeMenuHost::AddRef() {
    return InterlockedIncrement(&m_ref);    // Thread-safe increment
}

// Release: Decrement reference count when interface pointer is released
// Auto-delete object when reference count reaches zero
IFACEMETHODIMP_(ULONG) AwesomeMenuHost::Release() {
    ULONG c = InterlockedDecrement(&m_ref); // Thread-safe decrement
    if (!c) delete this;                   // Self-delete when no more references
    return c;
}

/*
 * File Path Extraction Helper
 * ============================
 * Extracts file/folder paths from Windows IDataObject (CF_HDROP format).
 * This is how we get the list of selected files when user right-clicks.
 *
 * CF_HDROP FORMAT:
 * - Windows standard format for drag-drop and clipboard file operations
 * - Contains array of file paths that were selected
 * - Used by Shell Extensions to determine context (what user clicked on)
 *
 * SECURITY MEASURES:
 * - Limits to 1000 files maximum to prevent memory exhaustion
 * - Validates path lengths to prevent buffer overflows
 * - Proper resource cleanup with GlobalLock/GlobalUnlock
 */
static void ExtractPathsFromDataObject(IDataObject* pdo, std::vector<std::wstring>& out) {
    if (!pdo) return;                     // No data object provided

    // Set up format descriptor for CF_HDROP (file drop format)
    FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg{};                      // Storage medium for data transfer

    // Request CF_HDROP data from the data object
    if (SUCCEEDED(pdo->GetData(&fe, &stg))) {
        HDROP hdrop = (HDROP)GlobalLock(stg.hGlobal);   // Lock global memory for access
        if (hdrop) {
            // Get count of files in the drop operation
            UINT c = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            if (c > 1000) c = 1000;           // SECURITY: Prevent excessive allocations

            // Extract each file path
            for (UINT i = 0; i < c; ++i) {
                wchar_t buf[MAX_PATH] = {0};  // Buffer for file path (zero-initialized)
                UINT pathLen = DragQueryFileW(hdrop, i, buf, ARRAYSIZE(buf));

                // SECURITY: Validate path length to prevent buffer overflows
                if (pathLen > 0 && pathLen < ARRAYSIZE(buf) - 1) {
                    out.emplace_back(buf);    // Add valid path to output vector
                }
            }
            GlobalUnlock(stg.hGlobal);        // Unlock global memory
        }
        ReleaseStgMedium(&stg);               // Release storage medium resources
    }
}

/*
 * IShellExtInit::Initialize Implementation
 * ========================================
 * Called by Windows to provide context information when shell extension is activated.
 * This tells us what the user right-clicked on and where they clicked.
 *
 * PARAMETERS:
 * - pidlFolder: Folder being viewed (for background right-clicks)
 * - pdtobj: Selected files/folders (for item right-clicks)
 * - hkeyProgID: File type registry key (usually unused)
 *
 * CONTEXT DETERMINATION LOGIC:
 * - If pidlFolder provided: Use as context directory (background click)
 * - If selection provided: Use parent folder of first selected item
 * - Store both directory context and selection for menu filtering
 *
 * This method captures a ContextSnapshot which is used throughout
 * the menu building and command execution process.
 */
IFACEMETHODIMP AwesomeMenuHost::Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject* pdtobj, HKEY) {
    try {
        m_context = buildContextSnapshot(pidlFolder, pdtobj);
        m_context.kind = detectContextKind(m_context);

        loadConfig();

        auto summary = std::format(L"Initialize captured kind {} with {} selections", static_cast<int>(m_context.kind), m_context.selection.size());
        logDebug(summary, LogCategory::Context);
        return S_OK;
    } catch (...) {
        logDebug(L"Initialize failed with an unexpected exception", LogCategory::Error);
        return E_FAIL;
    }
}

/*
 * Registry Reading Helper Function
 * ================================
 * Safe wrapper for reading string values from Windows registry.
 * Used for parsing existing shell extension registry entries.
 *
 * SECURITY FEATURES:
 * - Validates data type (REG_SZ or REG_EXPAND_SZ only)
 * - Prevents excessive memory allocation (32KB limit)
 * - Ensures proper null termination
 * - Handles variable-length registry strings safely
 *
 * RETURN: Registry string value, or empty string if error/not found
 */
static std::wstring RegReadSz(HKEY hKey, const wchar_t* name) {
    DWORD type = 0; DWORD cb = 0;

    // First query: get data type and size
    if (RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS) return L"";

    // SECURITY: Only accept string types
    if (type != REG_SZ && type != REG_EXPAND_SZ) return L"";

    // SECURITY: Prevent huge allocations (32KB limit)
    if (cb == 0 || cb > 32768) return L"";

    // Allocate string buffer with safety margin
    std::wstring s;
    s.resize((cb / sizeof(wchar_t)) + 1);     // Extra space for safety

    // Second query: get actual data
    DWORD actualSize = cb;
    if (RegQueryValueExW(hKey, name, nullptr, &type, (LPBYTE)s.data(), &actualSize) != ERROR_SUCCESS) return L"";

    // SECURITY: Ensure proper null termination and remove trailing nulls
    s.resize(actualSize / sizeof(wchar_t));
    while (!s.empty() && s.back() == L'\0') s.pop_back();

    return s;
}

/*
 * Hybrid Menu Configuration Loading System
 * ========================================
 * Combines always-present AwesomeMenu with user-extensible registry flyouts:
 *
 * ALWAYS PRESENT: Dynamic Context-Aware AwesomeMenu
 *   - Complete Visual Studio development environment setup
 *   - 15+ admin shell variants (VS 2019/2022/Insiders x86/x64 CMD/PS)
 *   - Context-aware content that adapts to what user right-clicked
 *   - Intelligent tool selection based on file type, location, drive type
 *
 * ADDITIONAL FLYOUTS: Registry Files as Separate Menus
 *   - Each .reg file in %APPDATA%\AwesomeMenuHost\menus\ becomes a separate flyout
 *   - Users can add specialized tool collections without affecting AwesomeMenu
 *   - Hot-reloadable without recompiling - just right-click to reload
 *   - Perfect for project-specific tools, team workflows, specialized utilities
 *
 * RESULT: Best of both worlds - reliable core functionality + unlimited extensibility
 */
void AwesomeMenuHost::loadConfig() {
    logDebug(L"loadConfig() starting - HYBRID SYSTEM", LogCategory::Menu);
    m_flyouts.clear();                    // Start with empty menu configuration
    m_activeFlyouts.clear();

    // NOTE: AwesomeMenu will be created dynamically based on context in QueryContextMenu
    // This ensures the menu content changes based on what the user right-clicked

    logDebug(std::format(L"After AwesomeMenu creation, flyouts count: {}", m_flyouts.size()), LogCategory::Menu);

    // THEN: Add registry files as separate flyouts
    try {
        logDebug(L"Loading registry files as separate flyouts...", LogCategory::Registry);
        loadRegistryFilesAsSeparateFlyouts(); // Each .reg file becomes its own flyout

        logDebug(std::format(L"After registry files, total flyouts count: {}", m_flyouts.size()), LogCategory::Registry);
    } catch (...) {
        logDebug(L"Exception in loadRegistryFilesAsSeparateFlyouts", LogCategory::Error);
        // If registry file loading fails, continue - AwesomeMenu is still available
        // This ensures robustness while maintaining core functionality
    }

    // EMERGENCY: Ultimate fallback if AwesomeMenu creation fails
    // if (m_flyouts.empty()) {
    //     // Create minimal emergency menu to ensure something always appears
    //     Flyout f{};
    //     f.name = L"Emergency";
    //     f.label = L"Awesome Menu";
    //     f.showIn = L"background;directory";

    //     f.items.push_back(FlyoutItem{
    //         .label = L"Open Terminal Here (Admin)",
    //         .command = L"wt.exe",
    //         .args = L"",
    //         .icon = L"",
    //         .workingDir = m_context.contextDir,
    //         .runAs = true,
    //         .section = L""
    //     });
    //     m_flyouts.push_back(std::move(f));
    // }
}

ContextSnapshot AwesomeMenuHost::buildContextSnapshot(PCIDLIST_ABSOLUTE pidlFolder, IDataObject* pdtobj) {
    ContextSnapshot snapshot;

    if (pidlFolder) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidlFolder, path)) {
            snapshot.contextDir.assign(path);
        }
    }

    ExtractPathsFromDataObject(pdtobj, snapshot.selection);

    if (snapshot.contextDir.empty() && !snapshot.selection.empty()) {
        const std::wstring& firstPath = snapshot.selection.front();
        if (firstPath.length() < MAX_PATH - 1) {
            wchar_t dir[MAX_PATH] = {0};
            if (wcscpy_s(dir, ARRAYSIZE(dir), firstPath.c_str()) == 0) {
                PathRemoveFileSpecW(dir);
                snapshot.contextDir.assign(dir);
            }
        }
    }

    return snapshot;
}

ContextKind AwesomeMenuHost::detectContextKind(const ContextSnapshot& snapshot) const {
    if (snapshot.hasSelection()) {
        if (snapshot.selection.size() > 1) {
            return ContextKind::Multi;
        }

        const std::wstring& itemPath = snapshot.primarySelection();

        if (PathIsDirectoryW(itemPath.c_str())) {
            ContextKind locationContext = getLocationContext(itemPath);
            if (locationContext != ContextKind::Background) {
                return locationContext;
            }

            ContextKind driveContext = getDriveTypeContext(itemPath);
            if (driveContext != ContextKind::Background) {
                return driveContext;
            }

            return ContextKind::Directory;
        }

        return getFileTypeContext(itemPath);
    }

    if (!snapshot.contextDir.empty()) {
        ContextKind locationContext = getLocationContext(snapshot.contextDir);
        if (locationContext != ContextKind::Background) {
            return locationContext;
        }
    }

    return ContextKind::Background;
}

/*
 * Unlimited Menu Test Generator
 * TEST STRUCTURE:
 * - Admin Tools Section: 8 elevated system utilities
 * - Development Section: 10 development environment tools
 * - System Utilities Section: 12 system management tools
 *
 * This function proves that our shell extension can display unlimited
 * cascading menu items, which is impossible with standard registry-based
 * shell extensions due to Windows' built-in parsing limitations.
 */
void AwesomeMenuHost::createUnlimitedTestMenu() {
    // Create test flyout with prominent labeling
    Flyout testFlyout{};
    testFlyout.name = L"UnlimitedTest";
    testFlyout.label = L"🚀 UNLIMITED TEST (25+ Items)";    // Prominent label with emoji
    testFlyout.showIn = L"background;directory;file";        // Show everywhere for testing

    // Create multiple sections with many items to test Windows limits
    // This demonstrates that IContextMenu3 can handle 25+ items while
    // registry-based shell extensions are limited to ~16 items

    // === SECTION 1: ADMIN TOOLS (8 items) ===
    // Elevated system administration utilities
    std::vector<std::wstring> adminTools = {
        L"Command Prompt (Admin)", L"PowerShell (Admin)", L"Windows Terminal (Admin)",
        L"Registry Editor", L"Device Manager", L"Event Viewer",
        L"System Configuration", L"Computer Management"
    };

    for (size_t i = 0; i < adminTools.size(); ++i) {
        FlyoutItem item{};
        item.label = adminTools[i];
        item.section = L"Admin Tools";               // Groups items with separator
        item.command = L"cmd.exe";
        item.args = L"/c echo \"Testing: " + adminTools[i] + L"\" & pause";
        item.workingDir = L"%DIR%";                  // Execute in context directory
        item.runAs = true;                           // Require UAC elevation
        testFlyout.items.push_back(std::move(item));
    }

    // === SECTION 2: DEVELOPMENT TOOLS (10 items) ===
    // Software development environment and tools
    std::vector<std::wstring> devTools = {
        L"Visual Studio Code", L"Visual Studio 2022", L"Visual Studio 2019",
        L"Notepad++", L"Git Bash", L"GitHub Desktop",
        L"Docker Desktop", L"Postman", L"Node.js REPL", L"Python REPL"
    };

    for (size_t i = 0; i < devTools.size(); ++i) {
        FlyoutItem item{};
        item.label = devTools[i];
        item.section = L"Development";               // Groups items with separator
        item.command = L"notepad.exe";
        item.args = L"\"" + testFlyout.label + L" - " + devTools[i] + L".txt\"";
        item.workingDir = L"%DIR%";                  // Execute in context directory
        item.runAs = false;                          // No elevation required
        testFlyout.items.push_back(std::move(item));
    }

    // === SECTION 3: SYSTEM UTILITIES (12 items) ===
    // System monitoring and maintenance tools
    std::vector<std::wstring> sysUtils = {
        L"Task Manager", L"Resource Monitor", L"System Information",
        L"Disk Cleanup", L"Disk Management", L"Services",
        L"Performance Monitor", L"Windows Memory Diagnostic",
        L"System File Checker", L"Check Disk", L"Defragment",
        L"Windows Update"
    };

    for (size_t i = 0; i < sysUtils.size(); ++i) {
        FlyoutItem item{};
        item.label = sysUtils[i];
        item.section = L"System Utilities";          // Groups items with separator
        item.command = L"powershell.exe";
        item.args = L"-Command \"Write-Host 'Testing unlimited menu: " + sysUtils[i] + L"'; Read-Host 'Press Enter to continue'\"";
        item.workingDir = L"%DIR%";                  // Execute in context directory
        item.runAs = false;                          // No elevation required
        testFlyout.items.push_back(std::move(item));
    }

    // Add the test flyout FIRST to make it prominent in context menu
    // This ensures the unlimited test menu appears at the top for easy verification
    m_flyouts.insert(m_flyouts.begin(), std::move(testFlyout));
}

/*
 * Complete AwesomeMenu Hardcoded Structure
 * =========================================
 * Recreates the complete AwesomeMenu configuration programmatically.
 * This bypasses Windows' 16-item registry parsing limit by building
 * the menu structure directly in code using IContextMenu3.
 *
 * MENU STRUCTURE:
 * - As Admin Submenu: 15+ elevated development shells
 *   - VS 2019/2022/Insiders x86/x64 Command/PowerShell variants
 *   - Standard elevated Command Prompt, PowerShell, Windows Terminal
 * - Direct Items: System utilities, development tools, applications
 *
 * This demonstrates unlimited cascading capability while providing
 * a comprehensive development environment context menu.
 */
void AwesomeMenuHost::createCompleteAwesomeMenu() {

    // Create root AwesomeMenu flyout
    Flyout awesomeMenu{};
    awesomeMenu.name = L"AwesomeMenu";
    awesomeMenu.label = L"Awesome Menu!!!";           // Enthusiastic branding
    awesomeMenu.showIn = L""; // Show in all contexts - items will be filtered individually

    // === AS ADMIN SUBMENU (Complete with ALL tools) ===
    // This submenu contains 15+ elevated development shells, demonstrating
    // unlimited cascading capability that's impossible with registry-based extensions
    Flyout asAdminSubmenu{};
    asAdminSubmenu.name = L"AsAdmin";
    asAdminSubmenu.label = L"As Admin";                // Submenu for elevated commands

    // Standard elevated Command Prompt
    FlyoutItem cmdAdmin{};
    cmdAdmin.label = L"Command Prompt";
    cmdAdmin.command = L"cmd.exe";
    cmdAdmin.args = L"";                              // No additional arguments
    cmdAdmin.workingDir = L"%DIR%";                   // Start in context directory
    cmdAdmin.runAs = true;                            // Require UAC elevation
    cmdAdmin.icon = L"cmd.exe";                       // Extract icon from executable
    asAdminSubmenu.items.push_back(std::move(cmdAdmin));

    // Standard elevated PowerShell
    FlyoutItem psAdmin{};
    psAdmin.label = L"PowerShell";
    psAdmin.command = L"powershell.exe";
    psAdmin.args = L"";                               // No additional arguments
    psAdmin.workingDir = L"%DIR%";                    // Start in context directory
    psAdmin.runAs = true;                             // Require UAC elevation
    psAdmin.icon = L"powershell.exe";                 // Extract icon from executable
    asAdminSubmenu.items.push_back(std::move(psAdmin));

    // Modern elevated Windows Terminal
    FlyoutItem wtAdmin{};
    wtAdmin.label = L"Windows Terminal";
    wtAdmin.command = L"wt.exe";
    wtAdmin.args = L"-d \"%DIR%\"";                   // Start in context directory
    wtAdmin.workingDir = L"%DIR%";
    wtAdmin.runAs = true;                             // Require UAC elevation
    wtAdmin.icon = L"wt.exe";                         // Extract icon from executable
    asAdminSubmenu.items.push_back(std::move(wtAdmin));

    // Visual Studio 2019 Developer Command Prompt (x64 architecture)
    FlyoutItem vs2019x64cmd{};
    vs2019x64cmd.label = L"VS 2019 DevShell (x64 CMD)";
    vs2019x64cmd.command = L"powershell.exe";
    vs2019x64cmd.args = L"-Command \"Start-Process 'C:\\tools\\Start-CmdDevShell.cmd' -ArgumentList '/2019' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vs2019x64cmd.workingDir = L"%DIR%";
    vs2019x64cmd.runAs = false;                       // PowerShell handles elevation
    asAdminSubmenu.items.push_back(std::move(vs2019x64cmd));

    // VS 2019 x64 PS
    FlyoutItem vs2019x64ps{};
    vs2019x64ps.label = L"VS 2019 DevShell (x64 PS)";
    vs2019x64ps.command = L"powershell.exe";
    vs2019x64ps.args = L"-Command \"Start-Process 'C:\\tools\\Start-VSDevShell.ps1' -ArgumentList '-x64', '-VSVersion', '2019' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vs2019x64ps.workingDir = L"%DIR%";
    vs2019x64ps.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vs2019x64ps));

    // VS 2019 x86 CMD
    FlyoutItem vs2019x86cmd{};
    vs2019x86cmd.label = L"VS 2019 DevShell (x86 CMD)";
    vs2019x86cmd.command = L"powershell.exe";
    vs2019x86cmd.args = L"-Command \"Start-Process 'C:\\tools\\Start-CmdDevShell.cmd' -ArgumentList '/x86', '/2019' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vs2019x86cmd.workingDir = L"%DIR%";
    vs2019x86cmd.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vs2019x86cmd));

    // VS 2019 x86 PS
    FlyoutItem vs2019x86ps{};
    vs2019x86ps.label = L"VS 2019 DevShell (x86 PS)";
    vs2019x86ps.command = L"powershell.exe";
    vs2019x86ps.args = L"-Command \"Start-Process 'C:\\tools\\Start-VSDevShell.ps1' -ArgumentList '/x86', '-VSVersion', '2019' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vs2019x86ps.workingDir = L"%DIR%";
    vs2019x86ps.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vs2019x86ps));

    // VS 2022 x64 CMD
    FlyoutItem vs2022x64cmd{};
    vs2022x64cmd.label = L"VS 2022 DevShell (x64 CMD)";
    vs2022x64cmd.command = L"powershell.exe";
    vs2022x64cmd.args = L"-Command \"Start-Process 'C:\\tools\\Start-CmdDevShell.cmd' -ArgumentList '/2022' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vs2022x64cmd.workingDir = L"%DIR%";
    vs2022x64cmd.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vs2022x64cmd));

    // VS 2022 x64 PS
    FlyoutItem vs2022x64ps{};
    vs2022x64ps.label = L"VS 2022 DevShell (x64 PS)";
    vs2022x64ps.command = L"powershell.exe";
    vs2022x64ps.args = L"-Command \"Start-Process 'C:\\tools\\Start-VSDevShell.ps1' -ArgumentList '-x64', '-VSVersion', '2022' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vs2022x64ps.workingDir = L"%DIR%";
    vs2022x64ps.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vs2022x64ps));

    // VS 2022 x86 CMD
    FlyoutItem vs2022x86cmd{};
    vs2022x86cmd.label = L"VS 2022 DevShell (x86 CMD)";
    vs2022x86cmd.command = L"powershell.exe";
    vs2022x86cmd.args = L"-Command \"Start-Process 'C:\\tools\\Start-CmdDevShell.cmd' -ArgumentList '/x86', '/2022' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vs2022x86cmd.workingDir = L"%DIR%";
    vs2022x86cmd.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vs2022x86cmd));

    // VS 2022 x86 PS
    FlyoutItem vs2022x86ps{};
    vs2022x86ps.label = L"VS 2022 DevShell (x86 PS)";
    vs2022x86ps.command = L"powershell.exe";
    vs2022x86ps.args = L"-Command \"Start-Process 'C:\\tools\\Start-VSDevShell.ps1' -ArgumentList '/x86', '-VSVersion', '2022' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vs2022x86ps.workingDir = L"%DIR%";
    vs2022x86ps.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vs2022x86ps));

    // VS Insiders x64 CMD
    FlyoutItem vsInsidersx64cmd{};
    vsInsidersx64cmd.label = L"VS Insiders DevShell (x64 CMD)";
    vsInsidersx64cmd.command = L"powershell.exe";
    vsInsidersx64cmd.args = L"-Command \"Start-Process 'C:\\tools\\Start-CmdDevShell.cmd' -ArgumentList '/Insiders' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vsInsidersx64cmd.workingDir = L"%DIR%";
    vsInsidersx64cmd.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vsInsidersx64cmd));

    // VS Insiders x64 PS
    FlyoutItem vsInsidersx64ps{};
    vsInsidersx64ps.label = L"VS Insiders DevShell (x64 PS)";
    vsInsidersx64ps.command = L"powershell.exe";
    vsInsidersx64ps.args = L"-Command \"Start-Process 'C:\\tools\\Start-VSDevShell.ps1' -ArgumentList '-x64', '-VSVersion', 'Insiders' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vsInsidersx64ps.workingDir = L"%DIR%";
    vsInsidersx64ps.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vsInsidersx64ps));

    // VS Insiders x86 CMD
    FlyoutItem vsInsidersx86cmd{};
    vsInsidersx86cmd.label = L"VS Insiders DevShell (x86 CMD)";
    vsInsidersx86cmd.command = L"powershell.exe";
    vsInsidersx86cmd.args = L"-Command \"Start-Process 'C:\\tools\\Start-CmdDevShell.cmd' -ArgumentList '/x86', '/Insiders' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vsInsidersx86cmd.workingDir = L"%DIR%";
    vsInsidersx86cmd.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vsInsidersx86cmd));

    // VS Insiders x86 PS
    FlyoutItem vsInsidersx86ps{};
    vsInsidersx86ps.label = L"VS Insiders DevShell (x86 PS)";
    vsInsidersx86ps.command = L"powershell.exe";
    vsInsidersx86ps.args = L"-Command \"Start-Process 'C:\\tools\\Start-VSDevShell.ps1' -ArgumentList '/x86', '-VSVersion', 'Insiders' -Verb RunAs -WorkingDirectory '%DIR%'\"";
    vsInsidersx86ps.workingDir = L"%DIR%";
    vsInsidersx86ps.runAs = false;
    asAdminSubmenu.items.push_back(std::move(vsInsidersx86ps));

    // Add the complete As Admin submenu to main menu
    // This creates unlimited cascading depth beyond Windows' 16-item registry limit
    awesomeMenu.subFlyouts.push_back(std::move(asAdminSubmenu));

    // === OTHER TOP-LEVEL ITEMS ===

    // Control Panel
    FlyoutItem controlPanel{};
    controlPanel.label = L"Control Panel";
    controlPanel.command = L"control.exe";
    controlPanel.workingDir = L"%DIR%";
    controlPanel.icon = L"control.exe";
    awesomeMenu.items.push_back(std::move(controlPanel));

    // Device Manager
    FlyoutItem deviceManager{};
    deviceManager.label = L"Device Manager";
    deviceManager.command = L"devmgmt.msc";
    deviceManager.workingDir = L"%DIR%";
    awesomeMenu.items.push_back(std::move(deviceManager));

    // MMC
    FlyoutItem mmc{};
    mmc.label = L"MMC";
    mmc.command = L"mmc.exe";
    mmc.workingDir = L"%DIR%";
    awesomeMenu.items.push_back(std::move(mmc));

    // Open VSCode Insiders
    FlyoutItem vscode{};
    vscode.label = L"Open in VSCode Insiders";
    vscode.command = L"code-insiders.exe";
    vscode.args = L"\"%DIR%\"";
    vscode.workingDir = L"%DIR%";
    vscode.icon = L"code-insiders.exe";
    awesomeMenu.items.push_back(std::move(vscode));

    // PowerShell 7
    FlyoutItem ps7{};
    ps7.label = L"PowerShell 7";
    ps7.command = L"pwsh.exe";
    ps7.workingDir = L"%DIR%";
    ps7.icon = L"pwsh.exe";
    awesomeMenu.items.push_back(std::move(ps7));

    // PowerShell ISE
    FlyoutItem psIse{};
    psIse.label = L"PowerShell ISE";
    psIse.command = L"powershell_ise.exe";
    psIse.workingDir = L"%DIR%";
    awesomeMenu.items.push_back(std::move(psIse));

    // Rocket League
    FlyoutItem rocketLeague{};
    rocketLeague.label = L"Rocket League";
    rocketLeague.command = L"explorer.exe";
    rocketLeague.args = L"\"steam://run/252950\"";
    rocketLeague.workingDir = L"%DIR%";
    awesomeMenu.items.push_back(std::move(rocketLeague));

    // Steam
    FlyoutItem steam{};
    steam.label = L"Steam";
    steam.command = L"pwsh.exe";
    steam.workingDir = L"%DIR%";
    awesomeMenu.items.push_back(std::move(steam));

    // Add the complete AwesomeMenu to flyouts
    m_flyouts.push_back(std::move(awesomeMenu));
}

/*
 * Context-Aware AwesomeMenu Creation System
 * ==========================================
 * Creates dynamic AwesomeMenu content based on what the user right-clicked.
 * This enables intelligent tool selection and context-appropriate options.
 */
Flyout AwesomeMenuHost::createContextAwareAwesomeMenu(const ContextSnapshot& snapshot) {
    logDebug(L"Creating context-aware AwesomeMenu", LogCategory::Menu);

    Flyout awesomeMenu{};
    awesomeMenu.name = L"AwesomeMenu";
    awesomeMenu.label = L"Awesome Menu";
    awesomeMenu.showIn = L"";

    ContextKind kind = snapshot.kind;

    switch (kind) {
        case ContextKind::Background:
        case ContextKind::Directory: {
            FlyoutItem cmdHere{};
            cmdHere.label = L"Command Prompt Here";
            cmdHere.command = L"cmd.exe";
            cmdHere.workingDir = L"%DIR%";
            cmdHere.icon = L"cmd.exe";
            awesomeMenu.items.push_back(std::move(cmdHere));

            FlyoutItem psHere{};
            psHere.label = L"PowerShell Here";
            psHere.command = L"powershell.exe";
            psHere.workingDir = L"%DIR%";
            psHere.icon = L"powershell.exe";
            awesomeMenu.items.push_back(std::move(psHere));

            FlyoutItem vsCode{};
            vsCode.label = L"Open in VS Code";
            vsCode.command = L"code";
            vsCode.args = L"\"%DIR%\"";
            vsCode.workingDir = L"%DIR%";
            awesomeMenu.items.push_back(std::move(vsCode));
            break;
        }

        case ContextKind::CodeFile: {
            FlyoutItem editVsCode{};
            editVsCode.label = L"Edit in VS Code";
            editVsCode.command = L"code";
            editVsCode.args = L"\"%SEL%\"";
            editVsCode.workingDir = L"%DIR%";
            awesomeMenu.items.push_back(std::move(editVsCode));

            FlyoutItem editNotepad{};
            editNotepad.label = L"Edit in Notepad++";
            editNotepad.command = L"notepad++.exe";
            editNotepad.args = L"\"%SEL%\"";
            editNotepad.workingDir = L"%DIR%";
            awesomeMenu.items.push_back(std::move(editNotepad));
            break;
        }

        case ContextKind::ImageFile: {
            FlyoutItem viewImage{};
            viewImage.label = L"Open with Paint";
            viewImage.command = L"mspaint.exe";
            viewImage.args = L"\"%SEL%\"";
            awesomeMenu.items.push_back(std::move(viewImage));
            break;
        }

        case ContextKind::ArchiveFile: {
            FlyoutItem extract{};
            extract.label = L"Extract Here";
            extract.command = L"7z.exe";
            extract.args = L"x \"%SEL%\" -o\"%DIR%\"";
            extract.workingDir = L"%DIR%";
            awesomeMenu.items.push_back(std::move(extract));
            break;
        }

        default: {
            FlyoutItem cmdHere{};
            cmdHere.label = L"Command Prompt Here";
            cmdHere.command = L"cmd.exe";
            cmdHere.workingDir = L"%DIR%";
            cmdHere.icon = L"cmd.exe";
            awesomeMenu.items.push_back(std::move(cmdHere));
            break;
        }
    }

    Flyout asAdminSubmenu{};
    asAdminSubmenu.name = L"AsAdmin";
    asAdminSubmenu.label = L"As Admin";

    FlyoutItem cmdAdmin{};
    cmdAdmin.label = L"Command Prompt (Admin)";
    cmdAdmin.command = L"cmd.exe";
    cmdAdmin.workingDir = L"%DIR%";
    cmdAdmin.runAs = true;
    cmdAdmin.icon = L"cmd.exe";
    asAdminSubmenu.items.push_back(std::move(cmdAdmin));

    FlyoutItem psAdmin{};
    psAdmin.label = L"PowerShell (Admin)";
    psAdmin.command = L"powershell.exe";
    psAdmin.workingDir = L"%DIR%";
    psAdmin.runAs = true;
    psAdmin.icon = L"powershell.exe";
    asAdminSubmenu.items.push_back(std::move(psAdmin));

    awesomeMenu.subFlyouts.push_back(std::move(asAdminSubmenu));

    auto summary = std::format(L"Context-aware menu built for kind {} with {} direct items", static_cast<int>(kind), awesomeMenu.items.size());
    logDebug(summary, LogCategory::Menu);

    return awesomeMenu;
}

UINT AwesomeMenuHost::buildContextMenu(const ContextSnapshot& snapshot, HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT uFlags) {
    UNREFERENCED_PARAMETER(indexMenu);
    UNREFERENCED_PARAMETER(uFlags);

    std::vector<Flyout> activeFlyouts = m_flyouts;
    Flyout contextAware = createContextAwareAwesomeMenu(snapshot);
    activeFlyouts.insert(activeFlyouts.begin(), std::move(contextAware));
    m_activeFlyouts = std::move(activeFlyouts);

    UINT idNext = idCmdFirst;
    m_idCmdFirst = idCmdFirst;
    m_idToPath.clear();

    auto containsCase = [](const std::wstring& hay, const wchar_t* needle) {
        if (hay.empty() || !needle) return false;
        std::wstring h = hay;
        for (auto& ch : h) ch = towlower(ch);
        std::wstring n = needle;
        for (auto& ch : n) ch = towlower(ch);
        return h.find(n) != std::wstring::npos;
    };

    auto matchShowIn = [&](const std::wstring& show) {
        if (show.empty()) return true;

        switch (snapshot.kind) {
            case ContextKind::Background: return containsCase(show, L"background");
            case ContextKind::Directory: return containsCase(show, L"directory");
            case ContextKind::File: return containsCase(show, L"file");
            case ContextKind::Multi: return containsCase(show, L"multi");

            case ContextKind::TextFile: return containsCase(show, L"text") || containsCase(show, L"file");
            case ContextKind::ImageFile: return containsCase(show, L"image") || containsCase(show, L"file");
            case ContextKind::ExecutableFile: return containsCase(show, L"executable") || containsCase(show, L"file");
            case ContextKind::ArchiveFile: return containsCase(show, L"archive") || containsCase(show, L"file");
            case ContextKind::DocumentFile: return containsCase(show, L"document") || containsCase(show, L"file");
            case ContextKind::CodeFile: return containsCase(show, L"code") || containsCase(show, L"file");
            case ContextKind::MediaFile: return containsCase(show, L"media") || containsCase(show, L"file");

            case ContextKind::HardDrive: return containsCase(show, L"drive") || containsCase(show, L"harddrive");
            case ContextKind::RemovableDrive: return containsCase(show, L"drive") || containsCase(show, L"removable");
            case ContextKind::NetworkDrive: return containsCase(show, L"drive") || containsCase(show, L"network");
            case ContextKind::OpticalDrive: return containsCase(show, L"drive") || containsCase(show, L"optical");

            case ContextKind::DesktopLocation: return containsCase(show, L"desktop") || containsCase(show, L"background");
            case ContextKind::DocumentsLocation: return containsCase(show, L"documents") || containsCase(show, L"directory");
            case ContextKind::SystemLocation: return containsCase(show, L"system") || containsCase(show, L"directory");
            case ContextKind::ProjectLocation: return containsCase(show, L"project") || containsCase(show, L"directory");
        }

        return true;
    };

    for (size_t f = 0; f < m_activeFlyouts.size(); ++f) {
        const auto& fly = m_activeFlyouts[f];
        if (!matchShowIn(fly.showIn)) {
            continue;
        }

        std::vector<UINT> rootPath = { static_cast<UINT>(f) };
        buildCascadingMenuFixed(hMenu, fly, idNext, rootPath, idCmdFirst);
    }

    UINT used = idNext - idCmdFirst;
    auto summary = std::format(L"buildContextMenu produced {} flyouts and {} command IDs", m_activeFlyouts.size(), used);
    logDebug(summary, LogCategory::Menu);
    return used;
}

/*
 * Registry Files Folder Path Generator
 * ====================================
 * Returns the standard folder path where registry files are stored:
 * %APPDATA%\AwesomeMenuHost\menus\
 *
 * This enables data-driven menu configuration by allowing users to
 * place .reg files in a known location for automatic loading.
 *
 * FOLDER STRUCTURE:
 * %APPDATA%\AwesomeMenuHost\menus\*.reg
 *   - Each .reg file becomes a separate menu flyout
 *   - Standard Windows registry export format supported
 *   - Hot-reloadable by restarting Explorer or re-registering extension
 */
std::wstring AwesomeMenuHost::getMenusFolder() const {
    PWSTR appDataPath = nullptr;

    // Get user's roaming AppData folder path using modern Windows API
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath) == S_OK) {
        std::wstring menusPath = appDataPath;
        CoTaskMemFree(appDataPath);           // Free COM-allocated memory
        menusPath += L"\\AwesomeMenuHost\\menus"; // Append our specific subfolder
        return menusPath;
    }

    return L"";                               // Fallback if AppData path unavailable
}

/*
 * Registry Files Loader System
 * =============================
 * Scans the menus folder for .reg files and loads them as menu configurations.
 * This implements the data-driven menu system that allows users to customize
 * menus without recompiling the shell extension.
 *
 * LOADING PROCESS:
 * 1. Get %APPDATA%\AwesomeMenuHost\menus folder path
 * 2. Create folder if it doesn't exist (first-run setup)
 * 3. Scan for *.reg files using Windows file enumeration
 * 4. Parse each .reg file into a Flyout structure
 * 5. Add valid flyouts to menu collection
 *
 * ERROR HANDLING:
 * - Gracefully handles missing folders
 * - Skips files that fail to parse
 * - Continues loading other files if one fails
 * - Falls back to hardcoded menus if no files found
 */
/*
 * Registry Files as Separate Flyouts System
 * ==========================================
 * This is the NEW hybrid approach: each .reg file becomes its own top-level flyout
 * rather than trying to parse complex registry structures into the main AwesomeMenu.
 *
 * BENEFITS:
 * - Clean separation: AwesomeMenu stays predictable, .reg files add functionality
 * - Simple parsing: Each file just needs basic commands, no complex hierarchy
 * - User-friendly: Easy to understand and organize specialized tool collections
 * - Extensible: Unlimited specialized menus without affecting core functionality
 */
void AwesomeMenuHost::loadRegistryFilesAsSeparateFlyouts() {
    std::wstring menusFolder = getMenusFolder();
    if (menusFolder.empty()) return;          // Can't determine AppData path

    // Check if menus folder exists, create if necessary
    DWORD attrs = GetFileAttributesW(menusFolder.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        // Folder doesn't exist, try to create it for first-run setup
        if (!CreateDirectoryW(menusFolder.c_str(), nullptr)) {
            return;                           // Can't create folder, abandon file loading
        }
    }

    std::set<std::wstring> currentFiles;

    std::wstring searchPattern = menusFolder + L"\\*.reg";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring fileName = findData.cFileName;
                std::wstring fullPath = menusFolder + L"\\" + fileName;
                currentFiles.insert(fileName);

                removeManagedEntriesForFile(fileName);

                std::vector<std::pair<std::wstring, std::wstring>> appliedEntries;
                if (applyRegistryFile(fullPath, fileName, appliedEntries)) {
                    storeManagedRegistryEntries(fileName, appliedEntries);

                    Flyout flyout{};
                    flyout.name = fileName.substr(0, fileName.find_last_of(L'.'));
                    flyout.label = flyout.name;
                    flyout.showIn = L"background;directory;file";

                    try {
                        parseRegistryFileSimplified(fullPath, flyout);
                        if (!flyout.items.empty() || !flyout.subFlyouts.empty()) {
                            m_flyouts.push_back(std::move(flyout));
                        }
                    } catch (...) {
                        logDebug(std::format(L"Failed to parse registry file {}", fileName), LogCategory::Error);
                    }
                }
            }
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);
    }

    purgeMissingRegistryFiles(currentFiles);
}

/*
 * Registry File Parser
 * ====================
 * Parses Windows .reg files to extract shell extension menu definitions.
 * Supports standard Windows Registry Editor export format with shell commands.
 *
 * SUPPORTED REGISTRY STRUCTURE:
 * [HKEY_CLASSES_ROOT\Directory\shell\MenuName]
 * "MUIVerb"="Display Name"
 * "Icon"="executable.exe"
 * "HasLUAShield"=dword:00000001
 *
 * [HKEY_CLASSES_ROOT\Directory\shell\MenuName\command]
 * @="command to execute"
 *
 * PARSING FEATURES:
 * - Handles Unicode BOM characters
 * - Extracts display labels, commands, icons, and elevation requirements
 * - Supports both MUIVerb and default value labels
 * - Automatically sets working directory to %DIR% placeholder
 * - Groups related commands into single flyout structure
 *
 * ERROR HANDLING:
 * - Gracefully handles malformed registry syntax
 * - Skips invalid entries and continues parsing
 * - Ensures safe string operations with bounds checking
 */
void AwesomeMenuHost::parseRegistryFile(const std::wstring& filePath, Flyout& targetFlyout) {
    // Open registry file for reading (Unicode support)
    std::wifstream file(filePath);
    if (!file.is_open()) return;              // File not accessible

    // Parser state variables
    std::wstring line;
    std::wstring currentKey;
    FlyoutItem currentItem;
    bool inShellKey = false;                  // Track if we're in a shell command key
    std::wstring currentLabel;

    // Parse file line by line
    while (std::getline(file, line)) {
        // Remove Unicode BOM (Byte Order Mark) if present
        if (!line.empty() && line[0] == 0xFEFF) {
            line = line.substr(1);
        }

        // Trim whitespace from both ends
        size_t start = line.find_first_not_of(L" \t\r\n");
        if (start != std::wstring::npos) {
            size_t end = line.find_last_not_of(L" \t\r\n");
            line = line.substr(start, end - start + 1);
        } else {
            line.clear();
        }

        // Skip empty lines and comments (lines starting with semicolon)
        if (line.empty() || line[0] == L';') continue;

        // Parse registry key sections [HKEY_CLASSES_ROOT\...]
        if (line[0] == L'[' && line.back() == L']') {
            // Save previous item if we have complete information
            if (inShellKey && !currentLabel.empty() && !currentItem.command.empty()) {
                currentItem.label = currentLabel;
                targetFlyout.items.push_back(std::move(currentItem));
                currentItem = {};             // Reset for next item
                currentLabel.clear();
            }

            // Extract key path (remove brackets)
            currentKey = line.substr(1, line.length() - 2);

            // Determine if this is a shell command key (not a \command subkey)
            inShellKey = (currentKey.find(L"\\shell\\") != std::wstring::npos) &&
                        (currentKey.find(L"\\command") == std::wstring::npos);
        }
        // Parse registry value assignments (name=value)
        else if (line.find(L'=') != std::wstring::npos) {
            size_t equalPos = line.find(L'=');
            std::wstring valueName = line.substr(0, equalPos);
            std::wstring valueData = line.substr(equalPos + 1);

            // Remove quotes from value name and data
            if (!valueName.empty() && valueName[0] == L'"' && valueName.back() == L'"') {
                valueName = valueName.substr(1, valueName.length() - 2);
            }
            if (!valueData.empty() && valueData[0] == L'"' && valueData.back() == L'"') {
                valueData = valueData.substr(1, valueData.length() - 2);
            }

            // Extract shell command information based on value name
            if (inShellKey) {
                if (valueName == L"MUIVerb" || valueName.empty()) {
                    // Handle command vs label distinction
                    if (valueName.empty() && currentKey.find(L"\\command") != std::wstring::npos) {
                        // This is the command to execute (default value of command key)
                        currentItem.command = valueData;
                        currentItem.workingDir = L"%DIR%"; // Set context directory placeholder
                    } else {
                        // This is the display label (MUIVerb or default value)
                        currentLabel = valueData;
                    }
                }
                else if (valueName == L"Icon") {
                    // Icon specification (usually executable path)
                    currentItem.icon = valueData;
                }
                else if (valueName == L"HasLUAShield") {
                    // UAC elevation shield indicator
                    currentItem.runAs = true;
                }
            }
        }
    }

    // Save the final item if we have complete information
    if (inShellKey && !currentLabel.empty() && !currentItem.command.empty()) {
        currentItem.label = currentLabel;
        targetFlyout.items.push_back(std::move(currentItem));
    }

    file.close();                             // Close file handle
}

/*
 * Simplified Registry File Parser for Separate Flyouts
 * =====================================================
 * A much simpler parser that extracts basic shell commands from .reg files
 * without trying to handle complex SubCommands or hierarchical structures.
 * Perfect for creating separate flyouts from user-provided registry files.
 *
 * SUPPORTED STRUCTURE:
 * [HKEY_*\Directory\shell\CommandName]
 * "MUIVerb"="Display Name"
 * "Icon"="executable.exe"
 * "HasLUAShield"=dword:00000001
 *
 * [HKEY_*\Directory\shell\CommandName\command]
 * @="command to execute"
 *
 * This approach focuses on extracting individual commands rather than
 * complex hierarchies, making it much more reliable for user-created files.
 */
void AwesomeMenuHost::parseRegistryFileSimplified(const std::wstring& filePath, Flyout& targetFlyout) {
    // Open registry file for reading (Unicode support)
    std::wifstream file(filePath);
    if (!file.is_open()) return;              // File not accessible

    std::wstring line;
    std::map<std::wstring, FlyoutItem> pendingItems; // Commands being built
    std::wstring currentCommand;              // Current command being processed
    bool inCommandKey = false;                // Are we in a \command key?

    // Parse file line by line with simplified logic
    while (std::getline(file, line)) {
        // Remove Unicode BOM and trim whitespace
        if (!line.empty() && line[0] == 0xFEFF) {
            line = line.substr(1);
        }

        // Trim whitespace
        size_t start = line.find_first_not_of(L" \t\r\n");
        if (start != std::wstring::npos) {
            size_t end = line.find_last_not_of(L" \t\r\n");
            line = line.substr(start, end - start + 1);
        } else {
            line.clear();
        }

        // Skip empty lines and comments
        if (line.empty() || line[0] == L';') continue;

        // Parse registry key sections [HKEY_...]
        if (line[0] == L'[' && line.back() == L']') {
            std::wstring keyPath = line.substr(1, line.length() - 2);

            // Reset state
            inCommandKey = false;
            currentCommand.clear();

            // Check if this is a shell command key
            size_t shellPos = keyPath.find(L"\\shell\\");
            if (shellPos != std::wstring::npos) {
                size_t commandPos = keyPath.find(L"\\command");

                if (commandPos != std::wstring::npos) {
                    // This is a command key - extract command name
                    inCommandKey = true;
                    size_t start = shellPos + 7; // Length of "\\shell\\"
                    size_t end = commandPos;
                    if (end > start) {
                        currentCommand = keyPath.substr(start, end - start);
                        // Initialize item if it doesn't exist
                        if (pendingItems.find(currentCommand) == pendingItems.end()) {
                            pendingItems[currentCommand] = FlyoutItem{};
                        }
                    }
                } else {
                    // This is a shell key (not command) - extract command name
                    size_t start = shellPos + 7; // Length of "\\shell\\"
                    currentCommand = keyPath.substr(start);
                    // Initialize item if it doesn't exist
                    if (pendingItems.find(currentCommand) == pendingItems.end()) {
                        pendingItems[currentCommand] = FlyoutItem{};
                    }
                }
            }
        }
        // Parse registry value assignments
        else if (line.find(L'=') != std::wstring::npos && !currentCommand.empty()) {
            size_t equalPos = line.find(L'=');
            std::wstring valueName = line.substr(0, equalPos);
            std::wstring valueData = line.substr(equalPos + 1);

            // Remove quotes from value name and data
            if (!valueName.empty() && valueName[0] == L'"' && valueName.back() == L'"') {
                valueName = valueName.substr(1, valueName.length() - 2);
            }
            if (!valueData.empty() && valueData[0] == L'"' && valueData.back() == L'"') {
                valueData = valueData.substr(1, valueData.length() - 2);
            }

            // Extract information based on value name
            auto& item = pendingItems[currentCommand];

            if (valueName == L"MUIVerb" || (valueName.empty() && !inCommandKey)) {
                // Display label
                item.label = valueData;
            }
            else if (valueName.empty() && inCommandKey) {
                // Command to execute
                item.command = valueData;
                item.workingDir = L"%DIR%"; // Set context directory placeholder
            }
            else if (valueName == L"Icon") {
                // Icon specification
                item.icon = valueData;
            }
            else if (valueName == L"HasLUAShield") {
                // UAC elevation required
                item.runAs = true;
            }
        }
    }

    // Add all completed items to the flyout
    for (const auto& pair : pendingItems) {
        const FlyoutItem& item = pair.second;
        // Only add items that have both label and command
        if (!item.label.empty() && !item.command.empty()) {
            targetFlyout.items.push_back(item);
        }
    }

    file.close();                             // Close file handle
}

void AwesomeMenuHost::loadAndConvertShellExtensions() {
    HKEY hAwesomeMenu{};
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Directory\\shell\\AwesomeMenu", 0, KEY_READ, &hAwesomeMenu) == ERROR_SUCCESS) {
        parseAwesomeMenuStructure(hAwesomeMenu);
        RegCloseKey(hAwesomeMenu);
    }
}

void AwesomeMenuHost::parseAwesomeMenuStructure(HKEY hAwesomeMenu) {
    // Create a flyout for AwesomeMenu
    Flyout flyout{};
    flyout.name = L"AwesomeMenu";
    flyout.label = RegReadSz(hAwesomeMenu, L"MUIVerb");
    if (flyout.label.empty()) {
        flyout.label = L"Awesome Menu";
    }
    flyout.showIn = L"background;directory"; // Default for Directory\shell entries

    // Read SubCommands to parse the hierarchical structure
    std::wstring subCommands = RegReadSz(hAwesomeMenu, L"SubCommands");
    if (!subCommands.empty()) {
        parseSubCommands(subCommands, L"Directory\\shell\\AwesomeMenu", flyout);
    }

    // Add the flyout if it has items
    if (!flyout.items.empty()) {
        m_flyouts.push_back(std::move(flyout));
    }
}

void AwesomeMenuHost::parseSubCommands(const std::wstring& subCommands, const std::wstring& basePath, Flyout& flyout) {
    // Split SubCommands by semicolon
    std::wstring commands = subCommands;
    size_t pos = 0;

    while (pos < commands.length() && pos < 10000) { // Prevent excessive processing
        size_t nextPos = commands.find(L';', pos);
        if (nextPos == std::wstring::npos) nextPos = commands.length();

        if (nextPos <= pos || (nextPos - pos) > 1000) { // Prevent huge substrings
            pos = nextPos + 1;
            continue;
        }

        std::wstring command = commands.substr(pos, nextPos - pos);

        // Remove whitespace safely
        size_t start = command.find_first_not_of(L" \t");
        if (start != std::wstring::npos) {
            size_t end = command.find_last_not_of(L" \t");
            if (end != std::wstring::npos && end >= start) {
                command = command.substr(start, end - start + 1);
            } else {
                command.clear();
            }
        } else {
            command.clear();
        }

        if (!command.empty()) {
            // For top-level SubCommands like "AwesomeMenu.AsAdmin", check if it's a section with nested commands
            size_t firstDot = command.find(L'.');
            if (firstDot != std::wstring::npos) {
                size_t secondDot = command.find(L'.', firstDot + 1);

                if (secondDot == std::wstring::npos) {
                    // This is a section (e.g., "AwesomeMenu.AsAdmin"), check for nested SubCommands
                    std::wstring sectionName = command.substr(firstDot + 1);
                    std::wstring sectionPath = basePath + L"\\shell\\" + sectionName;

                    HKEY hSection{};
                    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, sectionPath.c_str(), 0, KEY_READ, &hSection) == ERROR_SUCCESS) {
                        std::wstring nestedSubCommands = RegReadSz(hSection, L"SubCommands");
                        if (!nestedSubCommands.empty()) {
                            // Recursively parse nested SubCommands
                            parseSubCommands(nestedSubCommands, sectionPath, flyout);
                        }
                        RegCloseKey(hSection);
                    }
                } else {
                    // This is a direct command (e.g., "AwesomeMenu.AsAdmin.CMD")
                    parseDirectCommand(command, basePath, flyout);
                }
            }
        }

        pos = nextPos + 1;
    }
}

void AwesomeMenuHost::parseDirectCommand(const std::wstring& command, const std::wstring& basePath, Flyout& flyout) {
    // Extract the section and command name from "AwesomeMenu.AsAdmin.CMD"
    size_t lastDot = command.find_last_of(L'.');
    std::wstring commandName = (lastDot != std::wstring::npos) ? command.substr(lastDot + 1) : command;

    // Find the section (second-to-last part)
    std::wstring section;
    if (lastDot != std::wstring::npos) {
        size_t secondLastDot = command.find_last_of(L'.', lastDot - 1);
        if (secondLastDot != std::wstring::npos) {
            section = command.substr(secondLastDot + 1, lastDot - secondLastDot - 1);
        }
    }

    // Build registry path for this command
    // Convert "AwesomeMenu.AsAdmin.CMD" to "Directory\shell\AwesomeMenu\shell\AsAdmin\shell\CMD"
    std::wstring cmdPath = L"Directory\\shell\\AwesomeMenu\\shell";
    size_t dotPos = command.find(L'.', command.find(L'.') + 1); // Skip first dot (AwesomeMenu.)
    int pathDepth = 0; // Prevent excessive nesting
    while (dotPos != std::wstring::npos && pathDepth < 10) {
        size_t nextDot = command.find(L'.', dotPos + 1);
        if (dotPos + 1 >= command.length()) break; // Prevent out of bounds

        size_t partLen = (nextDot != std::wstring::npos) ? nextDot - dotPos - 1 : command.length() - dotPos - 1;
        if (partLen > 0 && partLen < 256) { // Reasonable part length
            std::wstring part = command.substr(dotPos + 1, partLen);
            if (cmdPath.length() + part.length() + 10 < 2000) { // Prevent excessive path length
                cmdPath += L"\\" + part;
                if (nextDot != std::wstring::npos) {
                    cmdPath += L"\\shell";
                }
            }
        }
        dotPos = nextDot;
        pathDepth++;
    }

    // Read the command details from registry
    HKEY hCommand{};
    std::wstring commandKeyPath = cmdPath + L"\\command";
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, commandKeyPath.c_str(), 0, KEY_READ, &hCommand) == ERROR_SUCCESS) {
        FlyoutItem item{};

        // Get the label from the parent key's MUIVerb
        HKEY hParent{};
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, cmdPath.c_str(), 0, KEY_READ, &hParent) == ERROR_SUCCESS) {
            item.label = RegReadSz(hParent, L"MUIVerb");
            if (item.label.empty()) {
                item.label = commandName; // Fallback to command name
            }
            item.icon = RegReadSz(hParent, L"Icon");
            RegCloseKey(hParent);
        }

        // Get the command string (default value)
        std::wstring fullCommand = RegReadSz(hCommand, L"");
        if (!fullCommand.empty() && fullCommand.length() < 2000) { // Reasonable command length
            // Parse the command - handle PowerShell wrapping if present
            if (fullCommand.find(L"powershell.exe") == 0) {
                // Extract actual command from PowerShell wrapper
                // "powershell.exe -Command "Start-Process 'cmd.exe' -Verb RunAs -WorkingDirectory '%1'""
                size_t startQuote = fullCommand.find(L"Start-Process '");
                if (startQuote != std::wstring::npos && startQuote < fullCommand.length() - 15) {
                    startQuote += 15; // Length of "Start-Process '"
                    size_t endQuote = fullCommand.find(L"'", startQuote);
                    if (endQuote != std::wstring::npos && endQuote > startQuote && (endQuote - startQuote) < 1000) {
                        item.command = fullCommand.substr(startQuote, endQuote - startQuote);

                        // Check for RunAs
                        if (fullCommand.find(L"-Verb RunAs") != std::wstring::npos) {
                            item.runAs = true;
                        }

                        // Set working directory placeholder
                        item.workingDir = L"%DIR%";
                    }
                }
            } else {
                // Direct command
                item.command = fullCommand;
                item.workingDir = L"%DIR%";
            }

            item.section = section;

            if (!item.label.empty() && !item.command.empty()) {
                flyout.items.push_back(std::move(item));
            }
        }

        RegCloseKey(hCommand);
    }
}

/*
 * Placeholder Expansion System
 * ============================
 * Replaces dynamic placeholders in command strings with actual runtime values.
 * This enables context-aware commands that adapt to the user's current selection.
 *
 * SUPPORTED PLACEHOLDERS:
 * - %DIR%: Current context directory (folder being viewed or parent of selection)
 * - %SEL%: Path of first selected file/folder (for single-item operations)
 *
 * SECURITY FEATURES:
 * - Limits replacement iterations to prevent infinite loops (100 max)
 * - Prevents excessive string growth (10KB limit)
 * - Safe string replacement algorithm
 *
 * USAGE EXAMPLES:
 * - Command: "cmd.exe", Args: "/k cd /d \"%DIR%\"" → Opens CMD in context folder
 * - Command: "notepad.exe", Args: "\"%SEL%\"" → Opens selected file in Notepad
 */
bool AwesomeMenuHost::expandPlaceholders(std::wstring& s) const {
    if (s.empty()) return false;

    // Safe string replacement helper with security limits
    auto replaceAll = [&](const std::wstring& from, const std::wstring& to) {
        size_t pos = 0;
        bool any = false;
        int replacements = 0;

        // Search and replace all occurrences
        while ((pos = s.find(from, pos)) != std::wstring::npos && replacements < 100) {
            // SECURITY: Prevent excessive string growth
            if (s.length() - from.size() + to.size() > 10000) break;

            s.replace(pos, from.size(), to);
            pos += to.size();                 // Move past replaced text
            any = true;
            replacements++;
        }
        return any;
    };

    // Replace placeholders with actual values
    if (m_context.hasSelection()) {
        replaceAll(L"%SEL%", m_context.primarySelection());
    }
    if (!m_context.contextDir.empty()) {
        replaceAll(L"%DIR%", m_context.contextDir);
    }

    return true;
}

/*
 * Menu Item Execution System
 * ===========================
 * Executes menu commands when user clicks menu items, with full support for:
 * - Placeholder expansion (%DIR%, %SEL%)
 * - UAC elevation (Run as Administrator)
 * - Working directory context
 * - Command line arguments
 *
 * EXECUTION PROCESS:
 * 1. Extract command, arguments, and working directory from menu item
 * 2. Expand dynamic placeholders with current context values
 * 3. Configure ShellExecuteEx structure for proper execution
 * 4. Handle UAC elevation if required ("runas" verb)
 * 5. Execute command and return status
 *
 * SECURITY CONSIDERATIONS:
 * - Uses ShellExecuteEx for secure command execution
 * - Proper UAC integration for elevated commands
 * - Safe placeholder expansion with bounds checking
 * - Working directory validation and context awareness
 */
HRESULT AwesomeMenuHost::runItem(const FlyoutItem& it) const {
    // Prepare execution parameters
    std::wstring exe = it.command;
    std::wstring args = it.args;
    std::wstring wdir = it.workingDir.empty() ? m_context.contextDir : it.workingDir;

    // Expand dynamic placeholders with current context
    expandPlaceholders(args);                 // Replace %DIR%, %SEL% in arguments
    expandPlaceholders(wdir);                 // Replace %DIR%, %SEL% in working directory

    // Configure ShellExecuteEx structure for command execution
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;             // Don't return until process starts
    sei.hwnd = nullptr;                       // No parent window
    sei.lpVerb = it.runAs ? L"runas" : nullptr; // Use "runas" for UAC elevation
    sei.lpFile = exe.c_str();                 // Executable to run
    sei.lpParameters = args.empty() ? nullptr : args.c_str(); // Command line arguments
    sei.lpDirectory = wdir.empty() ? nullptr : wdir.c_str();  // Working directory
    sei.nShow = SW_SHOWNORMAL;                // Show window normally

    // Execute the command
    if (!ShellExecuteExW(&sei)) {
        // Return Windows error code if execution fails
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;                              // Success
}

/*
 * IContextMenu3::QueryContextMenu Implementation
 * ==============================================
 * This is the CORE METHOD that builds unlimited cascading context menus.
 * Called by Windows when user right-clicks to build the context menu.
 *
 * THE BREAKTHROUGH:
 * Windows limits registry-based shell extensions to ~16 cascading items.
 * By implementing IContextMenu3 and building menus programmatically, we
 * bypass this limitation entirely and can create unlimited menu depth.
 *
 * PARAMETERS:
 * - hMenu: Existing context menu to add our items to
 * - indexMenu: Position where our items should be inserted
 * - idCmdFirst: First command ID we can use for our menu items
 * - idCmdLast: Last command ID available (unused, we calculate our range)
 * - uFlags: Context flags (background click, selection, etc.)
 *
 * CONTEXT FILTERING SYSTEM:
 * Menus can specify showIn="background;directory;file;multi" to control
 * when they appear based on what the user right-clicked on.
 *
 * MENU BUILDING PROCESS:
 * 1. Determine context type (background, directory, file, multi-selection)
 * 2. Filter flyouts based on their showIn specification
 * 3. Build unlimited cascading menus programmatically
 * 4. Store ID-to-path mapping for command execution
 * 5. Return count of menu items added
 */
IFACEMETHODIMP AwesomeMenuHost::QueryContextMenu(HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT, UINT uFlags) {
    try {
        if (uFlags & CMF_DEFAULTONLY) {
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        }

        ContextSnapshot snapshot = m_context;
        snapshot.kind = detectContextKind(snapshot);

        UINT used = buildContextMenu(snapshot, hMenu, indexMenu, idCmdFirst, uFlags);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, static_cast<USHORT>(used));
    } catch (...) {
        logDebug(L"QueryContextMenu failed with an unexpected exception", LogCategory::Error);
        return E_FAIL;
    }
}

/*
 * IContextMenu3::InvokeCommand Implementation
 * ===========================================
 * Called by Windows when user clicks a menu item. This method resolves
 * the menu command ID back to the actual command and executes it.
 *
 * COMMAND RESOLUTION PROCESS:
 * 1. Extract command ID from click event
 * 2. Look up ID in our ID-to-path mapping table
 * 3. Navigate through flyout structure using stored path
 * 4. Execute the found menu item with full context
 *
 * CRITICAL BUG FIX:
 * Windows gives us absolute IDs when building menus but sends relative IDs
 * when executing. We store relative IDs (id - idCmdFirst) to fix this mapping.
 *
 * SUPPORTED INVOCATION TYPES:
 * - Numeric command IDs (standard menu clicks)
 * - String verbs (not implemented, returns E_FAIL)
 * - Unicode vs ANSI command info structures
 */
IFACEMETHODIMP AwesomeMenuHost::InvokeCommand(LPCMINVOKECOMMANDINFO pici) {
    try {
        if (!pici) return E_INVALIDARG;         // Null parameter check

        // Detect Unicode vs ANSI command info structure
        bool isUnicode = (pici->cbSize == sizeof(CMINVOKECOMMANDINFOEX)) &&
                        (pici->fMask & CMIC_MASK_UNICODE);

        // Handle numeric command IDs (standard menu item clicks)
        if (!HIWORD(pici->lpVerb)) {
            UINT id = LOWORD(pici->lpVerb);     // Extract command ID

            // Look up command ID in our mapping table
            auto it = m_idToPath.find(id);
            if (it != m_idToPath.end()) {
                const auto& path = it->second;

                // Validate path has minimum required elements
                if (path.size() >= 3) {
                    // Navigate to the menu item using stored path
                    const FlyoutItem* item = findItemByPath(path);
                    if (item) {
                        // Execute the menu item command
                        return runItem(*item);
                    }
                }
            }

            // Command ID not found in mapping
            return E_FAIL;
        } else {
            // String verbs not implemented (we only use numeric IDs)
            (void)isUnicode;                    // Suppress unused variable warning
            return E_FAIL;
        }
    } catch (...) {
        // CRITICAL: Never let C++ exceptions escape from COM interface methods
        return E_FAIL;
    }
}

/*
 * IContextMenu3 Extended Interface Methods
 * ========================================
 * These methods provide advanced context menu functionality beyond basic
 * menu building and command execution.
 */

// GetCommandString: Provides help text and tooltips for menu items (not implemented)
IFACEMETHODIMP AwesomeMenuHost::GetCommandString(UINT_PTR, UINT, UINT*, LPSTR, UINT) {
    return E_NOTIMPL;                         // Not implemented - no tooltip support
}

// HandleMenuMsg: Advanced message handling for IContextMenu2 compatibility
IFACEMETHODIMP AwesomeMenuHost::HandleMenuMsg(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(uMsg); UNREFERENCED_PARAMETER(wParam); UNREFERENCED_PARAMETER(lParam);
    return S_OK;                              // Basic implementation for compatibility
}

// HandleMenuMsg2: Enhanced message handling for IContextMenu3 features
IFACEMETHODIMP AwesomeMenuHost::HandleMenuMsg2(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* plResult) {
    // Handle menu cleanup when popup menus are destroyed
    if (uMsg == WM_UNINITMENUPOPUP) {
        // CRITICAL: Clean up menu icon bitmaps to prevent memory leaks
        for (HBITMAP hb : m_menuBitmaps) {
            if (hb) DeleteObject(hb);         // Free bitmap resources
        }
        m_menuBitmaps.clear();                // Clear bitmap cache
    }

    if (plResult) *plResult = 0;              // Initialize result
    return S_OK;
}

UINT AwesomeMenuHost::buildCascadingMenu(HMENU hParentMenu, const Flyout& flyout, UINT& idNext, std::vector<UINT>& currentPath) {
    // Create submenu for this flyout
    HMENU hSubMenu = CreatePopupMenu();
    if (!hSubMenu) return 0;

    UINT menuIndex = 0;

    // Add all direct items first
    std::wstring lastSection;
    for (size_t i = 0; i < flyout.items.size(); ++i) {
        const auto& item = flyout.items[i];

        // Add section separator if different from last section
        if (!item.section.empty() && i > 0 && _wcsicmp(item.section.c_str(), lastSection.c_str()) != 0) {
            MENUITEMINFOW sep{};
            sep.cbSize = sizeof(sep);
            sep.fMask = MIIM_FTYPE;
            sep.fType = MFT_SEPARATOR;
            InsertMenuItemW(hSubMenu, menuIndex++, TRUE, &sep);
        }

        // Create menu item with icon support
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_ID | MIIM_STRING;
        mi.wID = idNext;
        mi.dwTypeData = const_cast<LPWSTR>(item.label.c_str());

        // Add icon if specified
        if (!item.icon.empty()) {
            HBITMAP hIcon = hbitmapFromIconSpec(item.icon, 16);
            if (hIcon) {
                mi.fMask |= MIIM_BITMAP;
                mi.hbmpItem = hIcon;
                m_menuBitmaps.push_back(hIcon); // Track for cleanup
            }
        }

        InsertMenuItemW(hSubMenu, menuIndex++, TRUE, &mi);

        // Store path to this item - DEBUG: log what we're storing
        std::vector<UINT> itemPath = currentPath;
        itemPath.push_back((UINT)i); // item index
        itemPath.push_back(0); // 0 = item (not subflyout)

        // DEBUG: Show what ID we're storing
    std::wstring storeMsg = L"STORING: ID=" + std::to_wstring(mi.wID) + L" for item: " + item.label;
    logDebug(storeMsg, LogCategory::Menu);

        m_idToPath[mi.wID] = itemPath; // Store using the actual menu ID!
        idNext++;

        lastSection = item.section;
    }

    // Add cascading submenus for nested flyouts
    for (size_t s = 0; s < flyout.subFlyouts.size(); ++s) {
        const auto& subFlyout = flyout.subFlyouts[s];

        // Add separator before submenus if we have items
        if (!flyout.items.empty() && s == 0) {
            MENUITEMINFOW sep{};
            sep.cbSize = sizeof(sep);
            sep.fMask = MIIM_FTYPE;
            sep.fType = MFT_SEPARATOR;
            InsertMenuItemW(hSubMenu, menuIndex++, TRUE, &sep);
        }

        // Create path for submenu
        std::vector<UINT> subPath = currentPath;
        subPath.push_back((UINT)s); // subflyout index
        subPath.push_back(1); // 1 = subflyout

        // Recursively build the cascading submenu
        buildCascadingMenu(hSubMenu, subFlyout, idNext, subPath);
    }

    // Add this submenu to parent menu
    MENUITEMINFOW root{};
    root.cbSize = sizeof(root);
    root.fMask = MIIM_STRING | MIIM_SUBMENU;
    root.hSubMenu = hSubMenu;
    root.dwTypeData = const_cast<LPWSTR>(flyout.label.c_str());
    InsertMenuItemW(hParentMenu, GetMenuItemCount(hParentMenu), TRUE, &root);

    return menuIndex;
}

/*
 * Fixed Cascading Menu Builder
 * =============================
 * This is the CORE INNOVATION that enables unlimited cascading menus.
 * Builds menu structures programmatically using IContextMenu3, bypassing
 * Windows' 16-item registry parsing limitation.
 *
 * KEY FEATURES:
 * - Unlimited menu depth and item count
 * - Proper command ID mapping for execution
 * - Icon support with bitmap caching
 * - Section separators for menu organization
 * - Recursive submenu building
 *
 * CRITICAL BUG FIX:
 * This "Fixed" version stores relative command IDs (id - idCmdFirst) instead
 * of absolute IDs to fix the "command not found" execution bug.
 *
 * MENU STRUCTURE BUILDING:
 * 1. Create popup submenu for flyout
 * 2. Add direct menu items with icons and separators
 * 3. Recursively build nested submenus
 * 4. Store ID-to-path mappings for command resolution
 * 5. Attach submenu to parent menu
 */
UINT AwesomeMenuHost::buildCascadingMenuFixed(HMENU hParentMenu, const Flyout& flyout, UINT& idNext, std::vector<UINT>& currentPath, UINT idCmdFirst) {
    // Create popup submenu for this flyout
    HMENU hSubMenu = CreatePopupMenu();
    if (!hSubMenu) return 0;                  // Failed to create submenu

    UINT menuIndex = 0;                       // Track menu item insertion position

    // === ADD DIRECT MENU ITEMS ===
    // Process all direct executable items in this flyout
    std::wstring lastSection;                 // Track sections for separator insertion
    for (size_t i = 0; i < flyout.items.size(); ++i) {
        const auto& item = flyout.items[i];

        // Add section separator if this item starts a new section
        if (!item.section.empty() && i > 0 &&
            _wcsicmp(item.section.c_str(), lastSection.c_str()) != 0) {
            MENUITEMINFOW sep{};
            sep.cbSize = sizeof(sep);
            sep.fMask = MIIM_FTYPE;
            sep.fType = MFT_SEPARATOR;            // Visual separator line
            InsertMenuItemW(hSubMenu, menuIndex++, TRUE, &sep);
        }

        // Create menu item structure
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_ID | MIIM_STRING;         // ID and text required
        mi.wID = idNext;                          // Unique command ID
        mi.dwTypeData = const_cast<LPWSTR>(item.label.c_str()); // Display text

        // Add icon if specified
        if (!item.icon.empty()) {
            HBITMAP hIcon = hbitmapFromIconSpec(item.icon, 16); // Extract 16x16 icon
            if (hIcon) {
                mi.fMask |= MIIM_BITMAP;          // Enable bitmap display
                mi.hbmpItem = hIcon;              // Set icon bitmap
                m_menuBitmaps.push_back(hIcon);   // Track for cleanup
            }
        }

        // Insert menu item into submenu
        InsertMenuItemW(hSubMenu, menuIndex++, TRUE, &mi);

        // === CRITICAL: STORE COMMAND ID MAPPING ===
        // Create path to this item for command resolution
        std::vector<UINT> itemPath = currentPath;
        itemPath.push_back((UINT)i);              // Item index within flyout
        itemPath.push_back(0);                    // 0 = item (not subflyout)

        // FIXED: Store relative ID to fix execution bug
        UINT relativeId = mi.wID - idCmdFirst;
        m_idToPath[relativeId] = itemPath;
        idNext++;                                 // Advance to next available ID

        lastSection = item.section;               // Update section tracking
    }

    // === ADD NESTED CASCADING SUBMENUS ===
    // This enables UNLIMITED menu depth, the core innovation!
    for (size_t s = 0; s < flyout.subFlyouts.size(); ++s) {
        const auto& subFlyout = flyout.subFlyouts[s];

        // Add separator before submenus if we have direct items
        if (!flyout.items.empty() && s == 0) {
            MENUITEMINFOW sep{};
            sep.cbSize = sizeof(sep);
            sep.fMask = MIIM_FTYPE;
            sep.fType = MFT_SEPARATOR;            // Visual separator line
            InsertMenuItemW(hSubMenu, menuIndex++, TRUE, &sep);
        }

        // Create path for submenu navigation
        std::vector<UINT> subPath = currentPath;
        subPath.push_back((UINT)s);               // Subflyout index
        subPath.push_back(1);                     // 1 = subflyout (not item)

        // RECURSIVE CALL: Build unlimited cascading submenu depth
        // This is what makes unlimited menus possible!
        buildCascadingMenuFixed(hSubMenu, subFlyout, idNext, subPath, idCmdFirst);
    }

    // === ATTACH SUBMENU TO PARENT ===
    // Add this complete submenu to the parent menu
    MENUITEMINFOW root{};
    root.cbSize = sizeof(root);
    root.fMask = MIIM_STRING | MIIM_SUBMENU;      // Text and submenu required
    root.hSubMenu = hSubMenu;                     // Attach our built submenu
    root.dwTypeData = const_cast<LPWSTR>(flyout.label.c_str()); // Display text

    // Insert at end of parent menu
    InsertMenuItemW(hParentMenu, GetMenuItemCount(hParentMenu), TRUE, &root);

    return menuIndex;                             // Return number of items added
}

/*
 * Menu Item Path Navigation System
 * ================================
 * Navigates through the flyout structure using a stored path to find
 * the specific menu item that was clicked.
 *
 * PATH FORMAT:
 * [flyoutIdx, index1, type1, index2, type2, ...]
 * - flyoutIdx: Index of root flyout in m_flyouts
 * - index: Index within current flyout (item or subflyout index)
 * - type: 0 = item (executable), 1 = subflyout (submenu)
 *
 * NAVIGATION ALGORITHM:
 * 1. Start at root flyout specified by flyoutIdx
 * 2. For each path segment, check type:
 *    - Type 1: Navigate deeper into subflyout
 *    - Type 0: Return the target executable item
 * 3. Validate all indices to prevent out-of-bounds access
 *
 * This system enables command resolution for unlimited menu depth.
 */
const FlyoutItem* AwesomeMenuHost::findItemByPath(const std::vector<UINT>& path) const {
    // Validate minimum path length (flyout + item + type)
    if (path.size() < 3) return nullptr;

    const auto& source = m_activeFlyouts.empty() ? m_flyouts : m_activeFlyouts;

    // Extract root flyout index
    UINT flyoutIdx = path[0];
    if (flyoutIdx >= source.size()) return nullptr;

    // Start navigation at root flyout
    const Flyout* currentFlyout = &source[flyoutIdx];

    // Navigate through the path to find the target item
    // Process path segments in pairs: [index, type]
    for (size_t i = 1; i < path.size(); i += 2) {
        if (i + 1 >= path.size()) break;          // Ensure we have both index and type

        UINT index = path[i];                     // Index within current flyout
        UINT isSubflyout = path[i + 1];           // Type: 0=item, 1=subflyout

        if (isSubflyout == 1) {
            // Navigate deeper into subflyout
            if (index >= currentFlyout->subFlyouts.size()) return nullptr;
            currentFlyout = &currentFlyout->subFlyouts[index];
        } else {
            // This is the final executable item
            if (index >= currentFlyout->items.size()) return nullptr;
            return &currentFlyout->items[index];   // Found target item
        }
    }

    return nullptr;                               // Path didn't lead to valid item
}

HBITMAP AwesomeMenuHost::hbitmapFromIconSpec(const std::wstring& spec, int sizePx) {
    if (spec.empty()) return nullptr;

    // Extract icon from executable
    HICON hIcon = nullptr;

    // Try to extract icon from the executable
    ExtractIconExW(spec.c_str(), 0, nullptr, &hIcon, 1);

    if (!hIcon) {
        // Fallback: try system icons
        if (spec == L"cmd.exe") {
            hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        } else if (spec == L"powershell.exe" || spec == L"pwsh.exe") {
            hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        } else {
            hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        }
    }

    if (!hIcon) return nullptr;

    // Convert icon to bitmap
    HDC hdc = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdc, sizePx, sizePx);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // Fill with transparent background
    RECT rect = {0, 0, sizePx, sizePx};
    FillRect(hdcMem, &rect, (HBRUSH)GetStockObject(WHITE_BRUSH));

    // Draw icon
    DrawIconEx(hdcMem, 0, 0, hIcon, sizePx, sizePx, 0, nullptr, DI_NORMAL);

    SelectObject(hdcMem, hOldBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdc);
    DestroyIcon(hIcon);

    return hBitmap;
}

bool AwesomeMenuHost::applyRegistryFile(const std::wstring& fullPath, const std::wstring& fileKey,
                                        std::vector<std::pair<std::wstring, std::wstring>>& recordedEntries) {
    recordedEntries.clear();

    std::wifstream file(fullPath);
    if (!file.is_open()) {
        logDebug(std::format(L"Unable to open registry file {}", fullPath), LogCategory::Error);
        return false;
    }

    std::wstring line;
    std::wstring currentKey;

    while (std::getline(file, line)) {
        if (!line.empty() && line[0] == 0xFEFF) {
            line.erase(line.begin());
        }
        trimInPlace(line);
        if (line.empty()) continue;
        if (line[0] == L';' || line[0] == L'#') continue;

        if (line.front() == L'[' && line.back() == L']') {
            currentKey = line.substr(1, line.size() - 2);
            trimInPlace(currentKey);
            continue;
        }

        size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }

        if (currentKey.empty()) {
            logDebug(std::format(L"Value encountered without key in {}", fullPath), LogCategory::Registry);
            continue;
        }

        std::wstring valueName = line.substr(0, equals);
        trimInPlace(valueName);
        if (valueName == L"@") {
            valueName.clear();
        } else if (valueName.size() >= 2 && valueName.front() == L'"' && valueName.back() == L'"') {
            valueName = valueName.substr(1, valueName.size() - 2);
        }

        std::wstring valueData = line.substr(equals + 1);
        trimInPlace(valueData);

        while (!valueData.empty() && valueData.back() == L'\\') {
            valueData.pop_back();
            trimInPlace(valueData);
            std::wstring continuation;
            if (!std::getline(file, continuation)) {
                break;
            }
            if (!continuation.empty() && continuation[0] == 0xFEFF) {
                continuation.erase(continuation.begin());
            }
            trimInPlace(continuation);
            valueData += continuation;
        }

        ParsedRegValue parsed;
        if (!parseRegistryValueData(valueData, parsed)) {
            logDebug(std::format(L"Unsupported registry value '{}' in file {}", line, fullPath), LogCategory::Registry);
            continue;
        }

        HKEY rootKey{};
        std::wstring subKey;
        std::wstring normalizedRoot;
        if (!splitRegistryPath(currentKey, rootKey, subKey, normalizedRoot)) {
            logDebug(std::format(L"Unsupported registry hive '{}' in file {}", currentKey, fullPath), LogCategory::Registry);
            continue;
        }

        if (parsed.type == REG_NONE && parsed.data.empty() && valueData == L"-") {
            if (!deleteRegistryValue(rootKey, subKey, valueName)) {
                logDebug(std::format(L"Failed to delete registry value '{}' in '{}'", valueName, currentKey), LogCategory::Registry);
            }
            continue;
        }

        HKEY hKey = nullptr;
        LONG status = RegCreateKeyExW(rootKey, subKey.empty() ? nullptr : subKey.c_str(), 0, nullptr, 0,
                                      KEY_SET_VALUE, nullptr, &hKey, nullptr);
        if (status != ERROR_SUCCESS) {
            logDebug(std::format(L"Failed to open registry key '{}' (error {})", currentKey, status), LogCategory::Registry);
            continue;
        }

        status = RegSetValueExW(hKey, valueName.empty() ? nullptr : valueName.c_str(), 0, parsed.type,
                                parsed.data.empty() ? nullptr : parsed.data.data(),
                                static_cast<DWORD>(parsed.data.size()));
        RegCloseKey(hKey);

        if (status != ERROR_SUCCESS) {
            logDebug(std::format(L"Failed to set registry value '{}' in '{}' (error {})", valueName, currentKey, status), LogCategory::Registry);
            continue;
        }

        std::wstring recordedKey = normalizedRoot;
        if (!subKey.empty()) {
            recordedKey += L"\\" + subKey;
        }
        recordedEntries.emplace_back(std::move(recordedKey), valueName);
    }

    return true;
}

void AwesomeMenuHost::storeManagedRegistryEntries(const std::wstring& fileKey,
                                     const std::vector<std::pair<std::wstring, std::wstring>>& entries) {
    HKEY root = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kManagedRegistryRoot, 0, nullptr, 0, KEY_WRITE, nullptr, &root, nullptr) != ERROR_SUCCESS) {
        return;
    }

    HKEY fileNode = nullptr;
    if (RegCreateKeyExW(root, fileKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &fileNode, nullptr) == ERROR_SUCCESS) {
        std::vector<std::wstring> flattened;
        flattened.reserve(entries.size());
        for (const auto& entry : entries) {
            flattened.push_back(entry.first + L"|" + entry.second);
        }
        std::vector<wchar_t> multi = vectorToMultiSz(flattened);
        RegSetValueExW(fileNode, L"Entries", 0, REG_MULTI_SZ,
                       reinterpret_cast<const BYTE*>(multi.data()),
                       static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
        RegCloseKey(fileNode);
    }

    RegCloseKey(root);
}

void AwesomeMenuHost::removeManagedEntriesForFile(const std::wstring& fileKey) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kManagedRegistryRoot, 0, KEY_READ | KEY_WRITE, &root) != ERROR_SUCCESS) {
        return;
    }

    HKEY fileNode = nullptr;
    if (RegOpenKeyExW(root, fileKey.c_str(), 0, KEY_READ | KEY_WRITE, &fileNode) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD dataSize = 0;
        if (RegQueryValueExW(fileNode, L"Entries", nullptr, &type, nullptr, &dataSize) == ERROR_SUCCESS && type == REG_MULTI_SZ && dataSize > sizeof(wchar_t)) {
            std::vector<wchar_t> buffer(dataSize / sizeof(wchar_t));
            if (RegQueryValueExW(fileNode, L"Entries", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(buffer.data()), &dataSize) == ERROR_SUCCESS) {
                auto entries = multiSzToVector(buffer.data(), buffer.size());
                for (const auto& entry : entries) {
                    size_t pipe = entry.find(L'|');
                    std::wstring keyPath = (pipe == std::wstring::npos) ? entry : entry.substr(0, pipe);
                    std::wstring valueName = (pipe == std::wstring::npos) ? std::wstring() : entry.substr(pipe + 1);

                    HKEY rootKey{};
                    std::wstring subKey;
                    std::wstring normalized;
                    if (splitRegistryPath(keyPath, rootKey, subKey, normalized)) {
                        deleteRegistryValue(rootKey, subKey, valueName);
                    }
                }
            }
        }

        RegCloseKey(fileNode);
        RegDeleteTreeW(root, fileKey.c_str());
    }

    RegCloseKey(root);
}

void AwesomeMenuHost::purgeMissingRegistryFiles(const std::set<std::wstring>& currentFiles) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kManagedRegistryRoot, 0, KEY_READ, &root) != ERROR_SUCCESS) {
        return;
    }

    std::vector<std::wstring> trackedFiles;
    DWORD index = 0;
    wchar_t nameBuffer[256];
    while (true) {
        DWORD nameLen = ARRAYSIZE(nameBuffer);
        LONG status = RegEnumKeyExW(root, index, nameBuffer, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status == ERROR_SUCCESS) {
            trackedFiles.emplace_back(nameBuffer, nameLen);
        }
        ++index;
    }

    RegCloseKey(root);

    for (const auto& tracked : trackedFiles) {
        if (!currentFiles.contains(tracked)) {
            removeManagedEntriesForFile(tracked);
        }
    }
}