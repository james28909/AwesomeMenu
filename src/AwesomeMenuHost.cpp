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
#include <sstream>           // String composition helpers
#include <cstring>           // memcpy for registry value buffers
#include <cstdlib>           // wcstoul helpers
#include <optional>          // std::optional for game shortcut detection
#include <intshcut.h>        // IUniformResourceLocatorW for URL-type shortcuts

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
    if (buffer.size() % sizeof(wchar_t) != 0)
        buffer.push_back(0); // pad to wchar_t boundary
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

// ---------------------------------------------------------------------------
// Tool detection helpers
// ---------------------------------------------------------------------------

static bool pathExists(const wchar_t* path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring findOnPath(const wchar_t* exeName) {
    wchar_t buf[MAX_PATH] = {};
    if (wcscpy_s(buf, exeName) == 0 && PathFindOnPathW(buf, nullptr))
        return buf;
    return {};
}

static std::wstring getKnownFolder(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr;
    if (SHGetKnownFolderPath(id, 0, nullptr, &p) == S_OK) {
        std::wstring result = p;
        CoTaskMemFree(p);
        return result;
    }
    return {};
}

struct ToolPaths {
    std::wstring wt;           // Windows Terminal
    std::wstring pwsh;         // PowerShell 7
    std::wstring codeInsiders; // VS Code Insiders
    std::wstring code;         // VS Code
    std::wstring cursor;       // Cursor
    std::wstring zed;          // Zed
    std::wstring sublimeText;  // Sublime Text
    std::wstring devenv;       // Visual Studio 2022 IDE
    std::wstring vs2022Root;   // VS 2022 install root (for dev shells)
    std::wstring git;          // Git
    std::wstring gitBash;      // Git Bash
    std::wstring notepadPP;    // Notepad++
    std::wstring sevenZip;     // 7-Zip
    std::wstring python;       // Python interpreter
    std::wstring node;         // Node.js
    std::wstring pycharm;      // PyCharm (JetBrains)
    std::wstring rider;        // Rider (JetBrains)
    std::wstring webStorm;     // WebStorm (JetBrains)
    std::wstring clion;        // CLion (JetBrains)
    std::wstring vlc;          // VLC media player
    std::wstring winMerge;     // WinMerge diff tool
    std::wstring hxd;          // HxD hex editor
};

static ToolPaths detectTools() {
    // Cache result — detection only runs once per DLL lifetime
    static ToolPaths s_cached = []() -> ToolPaths {
        ToolPaths t;

        std::wstring local  = getKnownFolder(FOLDERID_LocalAppData);
        std::wstring pf     = getKnownFolder(FOLDERID_ProgramFiles);
        std::wstring pfx86  = getKnownFolder(FOLDERID_ProgramFilesX86);

        auto tryPath = [](std::wstring& dest, const std::wstring& path) {
            if (dest.empty() && pathExists(path.c_str()))
                dest = path;
        };

        // Windows Terminal
        t.wt = findOnPath(L"wt.exe");

        // PowerShell 7
        t.pwsh = findOnPath(L"pwsh.exe");
        if (!pf.empty()) tryPath(t.pwsh, pf + L"\\PowerShell\\7\\pwsh.exe");

        // VS Code Insiders
        t.codeInsiders = findOnPath(L"code-insiders.exe");
        if (!local.empty()) tryPath(t.codeInsiders, local + L"\\Programs\\Microsoft VS Code Insiders\\Code - Insiders.exe");

        // VS Code
        t.code = findOnPath(L"code.exe");
        if (!local.empty()) tryPath(t.code, local + L"\\Programs\\Microsoft VS Code\\Code.exe");

        // Cursor
        t.cursor = findOnPath(L"cursor.exe");
        if (!local.empty()) {
            tryPath(t.cursor, local + L"\\Programs\\cursor\\Cursor.exe");
            tryPath(t.cursor, local + L"\\Programs\\Cursor\\Cursor.exe");
        }

        // Zed
        t.zed = findOnPath(L"zed.exe");
        if (t.zed.empty() && !local.empty()) {
            tryPath(t.zed, local + L"\\Programs\\Zed\\bin\\zed.exe");
            tryPath(t.zed, local + L"\\Programs\\Zed\\zed.exe");
        }

        // Sublime Text
        t.sublimeText = findOnPath(L"subl.exe");
        if (t.sublimeText.empty()) {
            if (!pf.empty())    tryPath(t.sublimeText, pf    + L"\\Sublime Text\\subl.exe");
            if (!pfx86.empty()) tryPath(t.sublimeText, pfx86 + L"\\Sublime Text\\subl.exe");
            if (!pf.empty())    tryPath(t.sublimeText, pf    + L"\\Sublime Text 3\\subl.exe");
        }

        // Git
        t.git = findOnPath(L"git.exe");
        if (!pf.empty())    tryPath(t.git, pf    + L"\\Git\\bin\\git.exe");
        if (!pfx86.empty()) tryPath(t.git, pfx86 + L"\\Git\\bin\\git.exe");

        // Git Bash
        if (!pf.empty())    tryPath(t.gitBash, pf    + L"\\Git\\git-bash.exe");
        if (!pfx86.empty()) tryPath(t.gitBash, pfx86 + L"\\Git\\git-bash.exe");

        // Notepad++
        t.notepadPP = findOnPath(L"notepad++.exe");
        if (!pf.empty())    tryPath(t.notepadPP, pf    + L"\\Notepad++\\notepad++.exe");
        if (!pfx86.empty()) tryPath(t.notepadPP, pfx86 + L"\\Notepad++\\notepad++.exe");

        // 7-Zip
        t.sevenZip = findOnPath(L"7z.exe");
        if (!pf.empty())    tryPath(t.sevenZip, pf    + L"\\7-Zip\\7z.exe");
        if (!pfx86.empty()) tryPath(t.sevenZip, pfx86 + L"\\7-Zip\\7z.exe");

        // Python
        t.python = findOnPath(L"python.exe");
        if (t.python.empty()) t.python = findOnPath(L"py.exe");
        if (t.python.empty() && !local.empty()) {
            // Common Python Launcher / MS Store install location
            tryPath(t.python, local + L"\\Programs\\Python\\Launcher\\py.exe");
        }

        // Node.js
        t.node = findOnPath(L"node.exe");
        if (t.node.empty() && !pf.empty())
            tryPath(t.node, pf + L"\\nodejs\\node.exe");

        // JetBrains Toolbox scripts
        std::wstring jbScripts = local.empty() ? L"" : (local + L"\\JetBrains\\Toolbox\\scripts");

        // PyCharm
        t.pycharm = findOnPath(L"pycharm.cmd");
        if (t.pycharm.empty() && !jbScripts.empty()) tryPath(t.pycharm, jbScripts + L"\\pycharm.cmd");

        // Rider
        t.rider = findOnPath(L"rider.cmd");
        if (t.rider.empty() && !jbScripts.empty()) tryPath(t.rider, jbScripts + L"\\rider.cmd");

        // WebStorm
        t.webStorm = findOnPath(L"webstorm.cmd");
        if (t.webStorm.empty() && !jbScripts.empty()) tryPath(t.webStorm, jbScripts + L"\\webstorm.cmd");

        // CLion
        t.clion = findOnPath(L"clion.cmd");
        if (t.clion.empty() && !jbScripts.empty()) tryPath(t.clion, jbScripts + L"\\clion.cmd");

        // VLC
        t.vlc = findOnPath(L"vlc.exe");
        if (t.vlc.empty()) {
            if (!pf.empty())    tryPath(t.vlc, pf    + L"\\VideoLAN\\VLC\\vlc.exe");
            if (!pfx86.empty()) tryPath(t.vlc, pfx86 + L"\\VideoLAN\\VLC\\vlc.exe");
        }

        // WinMerge
        t.winMerge = findOnPath(L"WinMergeU.exe");
        if (t.winMerge.empty()) {
            if (!pf.empty())    tryPath(t.winMerge, pf    + L"\\WinMerge\\WinMergeU.exe");
            if (!pfx86.empty()) tryPath(t.winMerge, pfx86 + L"\\WinMerge\\WinMergeU.exe");
        }

        // HxD hex editor
        t.hxd = findOnPath(L"HxD.exe");
        if (t.hxd.empty()) {
            if (!pf.empty())    tryPath(t.hxd, pf    + L"\\HxD\\HxD.exe");
            if (!pfx86.empty()) tryPath(t.hxd, pfx86 + L"\\HxD\\HxD.exe");
        }

        // Visual Studio 2022 (check all editions)
        if (!pf.empty()) {
            static const wchar_t* editions[] = { L"Professional", L"Enterprise", L"Community", L"BuildTools" };
            for (const wchar_t* ed : editions) {
                std::wstring root   = pf + L"\\Microsoft Visual Studio\\2022\\" + ed;
                std::wstring devenv = root + L"\\Common7\\IDE\\devenv.exe";
                if (pathExists(devenv.c_str())) {
                    t.devenv     = devenv;
                    t.vs2022Root = root;
                    break;
                }
            }
        }

        return t;
    }();
    return s_cached;
}

// ---------------------------------------------------------------------------
// Dynamic "Open With" registry lookup
// ---------------------------------------------------------------------------

// Inline registry string reader — RegReadSz is defined after the namespace closes.
static std::wstring readRegStr(HKEY hKey, const wchar_t* name) {
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS) return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
    if (cb == 0 || cb > 32768) return {};
    std::wstring s(cb / sizeof(wchar_t) + 1, L'\0');
    DWORD sz = cb;
    if (RegQueryValueExW(hKey, name, nullptr, &type, (LPBYTE)s.data(), &sz) != ERROR_SUCCESS) return {};
    s.resize(sz / sizeof(wchar_t));
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

struct OpenWithEntry {
    std::wstring displayName;
    std::wstring exePath;
};

// Parse the executable path out of a shell command string.
// Handles: "C:\path\app.exe" "%1"  and  C:\path\app.exe %1
static std::wstring extractExeFromCommand(const std::wstring& cmd) {
    if (cmd.empty()) return {};
    if (cmd[0] == L'"') {
        size_t end = cmd.find(L'"', 1);
        if (end != std::wstring::npos) return cmd.substr(1, end - 1);
    } else {
        size_t sp = cmd.find(L' ');
        return (sp != std::wstring::npos) ? cmd.substr(0, sp) : cmd;
    }
    return {};
}

// Resolve a bare exe name to a full path via PATH; absolute paths are returned as-is.
static std::wstring resolveExePath(const std::wstring& nameOrPath) {
    if (nameOrPath.empty()) return {};
    if (nameOrPath.size() > 2 && nameOrPath[1] == L':')
        return pathExists(nameOrPath.c_str()) ? nameOrPath : L"";
    return findOnPath(nameOrPath.c_str());
}

// Return the filename without extension from a full path.
static std::wstring exeBaseName(const std::wstring& exePath) {
    size_t slash = exePath.find_last_of(L"\\/");
    std::wstring name = (slash != std::wstring::npos) ? exePath.substr(slash + 1) : exePath;
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.erase(dot);
    return name;
}

// Look up HKCR\Applications\<exe.exe>\FriendlyAppName for a display name.
static std::wstring getFriendlyNameForExe(const std::wstring& exeFileName) {
    std::wstring key = L"Applications\\" + exeFileName;
    HKEY h{};
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return {};
    auto name = readRegStr(h, L"FriendlyAppName");
    RegCloseKey(h);
    return name;
}

// Return all applications registered to open files with the given extension (no dot).
// Results are cached per extension for the lifetime of the DLL.
static std::vector<OpenWithEntry> queryOpenWith(const std::wstring& ext) {
    static std::map<std::wstring, std::vector<OpenWithEntry>> s_cache;
    {
        auto it = s_cache.find(ext);
        if (it != s_cache.end()) return it->second;
    }

    std::vector<OpenWithEntry> results;
    std::set<std::wstring> seen;

    static const wchar_t* kBlocklist[] = { L"rundll32.exe", L"dllhost.exe", L"msiexec.exe" };

    auto toLower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    };

    auto isBlocked = [&](const std::wstring& base) {
        std::wstring l = toLower(base) + L".exe";
        for (const auto* b : kBlocklist) { if (l == b) return true; }
        return false;
    };

    auto addEntry = [&](std::wstring exePath, std::wstring displayName) {
        if (exePath.empty() || !pathExists(exePath.c_str())) return;
        std::wstring base = exeBaseName(exePath);
        if (isBlocked(base)) return;
        std::wstring key = toLower(exePath);
        if (seen.count(key)) return;
        seen.insert(key);

        if (displayName.empty() || toLower(displayName) == L"open") {
            // Derive from  FriendlyAppName or exe basename
            size_t sl = exePath.find_last_of(L"\\/");
            std::wstring exeFile = (sl != std::wstring::npos) ? exePath.substr(sl + 1) : exePath;
            displayName = getFriendlyNameForExe(exeFile);
            if (displayName.empty()) displayName = base;
        }
        results.push_back({ std::move(displayName), std::move(exePath) });
    };

    // Resolve exe + display name from a ProgID
    auto addFromProgId = [&](const std::wstring& progId) {
        if (progId.empty()) return;
        std::wstring cmdPath = progId + L"\\shell\\open\\command";
        HKEY hCmd{};
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, cmdPath.c_str(), 0, KEY_READ, &hCmd) != ERROR_SUCCESS) return;
        std::wstring cmd = readRegStr(hCmd, nullptr);
        RegCloseKey(hCmd);
        if (cmd.empty()) return;

        std::wstring exePath = resolveExePath(extractExeFromCommand(cmd));

        // Pass empty display name — addEntry derives it from getFriendlyNameForExe.
        // The ProgID (default) value is the file type description ("Text Source File"),
        // not the app name, so it must not be used here.
        addEntry(std::move(exePath), L"");
    };

    std::wstring extKey = L"." + ext;
    HKEY hExt{};
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, extKey.c_str(), 0, KEY_READ, &hExt) != ERROR_SUCCESS) {
        s_cache[ext] = results;
        return results;
    }

    // 1. Default ProgID
    addFromProgId(readRegStr(hExt, nullptr));

    // 2. OpenWithProgids — value names are ProgIDs
    {
        HKEY hOwp{};
        if (RegOpenKeyExW(hExt, L"OpenWithProgids", 0, KEY_READ, &hOwp) == ERROR_SUCCESS) {
            DWORD idx = 0;
            wchar_t vName[256]; DWORD vLen = ARRAYSIZE(vName);
            while (RegEnumValueW(hOwp, idx, vName, &vLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                if (vLen > 0) addFromProgId(vName);
                ++idx; vLen = ARRAYSIZE(vName);
            }
            RegCloseKey(hOwp);
        }
    }

    // 3. OpenWithList — sub-key names are exe filenames
    {
        HKEY hOwl{};
        if (RegOpenKeyExW(hExt, L"OpenWithList", 0, KEY_READ, &hOwl) == ERROR_SUCCESS) {
            DWORD idx = 0;
            wchar_t subKey[256]; DWORD subLen = ARRAYSIZE(subKey);
            while (RegEnumKeyExW(hOwl, idx, subKey, &subLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                std::wstring exeName(subKey, subLen);
                // Try Applications\<exe>\shell\open\command first
                std::wstring cmdPath = L"Applications\\" + exeName + L"\\shell\\open\\command";
                HKEY hCmd{};
                std::wstring exePath;
                if (RegOpenKeyExW(HKEY_CLASSES_ROOT, cmdPath.c_str(), 0, KEY_READ, &hCmd) == ERROR_SUCCESS) {
                    exePath = resolveExePath(extractExeFromCommand(readRegStr(hCmd, nullptr)));
                    RegCloseKey(hCmd);
                }
                if (exePath.empty()) exePath = resolveExePath(exeName);
                addEntry(std::move(exePath), L"");
                ++idx; subLen = ARRAYSIZE(subKey);
            }
            RegCloseKey(hOwl);
        }
    }

    RegCloseKey(hExt);

    // 4. HKCU FileExts — per-user "Open With" history (not visible via HKCR)
    std::wstring feBase = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\." + ext;
    auto addFromOwlKey = [&](HKEY hOwl) {
        DWORD idx = 0;
        wchar_t vName[4]; DWORD vLen = ARRAYSIZE(vName);
        wchar_t vData[MAX_PATH]; DWORD vDataLen = sizeof(vData);
        DWORD type = 0;
        while (RegEnumValueW(hOwl, idx, vName, &vLen, nullptr, &type,
                             (BYTE*)vData, &vDataLen) == ERROR_SUCCESS) {
            if (type == REG_SZ && vLen > 0 && vName[0] != 'M') { // skip MRUList
                std::wstring exeName(vData, vDataLen / sizeof(wchar_t));
                exeName.erase(exeName.find_last_not_of(L'\0') + 1); // trim nulls
                std::wstring cmdPath = L"Applications\\" + exeName + L"\\shell\\open\\command";
                HKEY hCmd{};
                std::wstring exePath;
                if (RegOpenKeyExW(HKEY_CLASSES_ROOT, cmdPath.c_str(), 0, KEY_READ, &hCmd) == ERROR_SUCCESS) {
                    exePath = resolveExePath(extractExeFromCommand(readRegStr(hCmd, nullptr)));
                    RegCloseKey(hCmd);
                }
                if (exePath.empty()) exePath = resolveExePath(exeName);
                addEntry(std::move(exePath), L"");
            }
            ++idx; vLen = ARRAYSIZE(vName); vDataLen = sizeof(vData);
        }
    };
    {
        HKEY hFe{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, feBase.c_str(), 0, KEY_READ, &hFe) == ERROR_SUCCESS) {
            // OpenWithProgids
            HKEY hOwp{};
            if (RegOpenKeyExW(hFe, L"OpenWithProgids", 0, KEY_READ, &hOwp) == ERROR_SUCCESS) {
                DWORD idx = 0;
                wchar_t vName[256]; DWORD vLen = ARRAYSIZE(vName);
                while (RegEnumValueW(hOwp, idx, vName, &vLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                    if (vLen > 0) addFromProgId(vName);
                    ++idx; vLen = ARRAYSIZE(vName);
                }
                RegCloseKey(hOwp);
            }
            // OpenWithList (MRU exe names stored as value data, not key names)
            HKEY hOwl{};
            if (RegOpenKeyExW(hFe, L"OpenWithList", 0, KEY_READ, &hOwl) == ERROR_SUCCESS) {
                addFromOwlKey(hOwl);
                RegCloseKey(hOwl);
            }
            RegCloseKey(hFe);
        }
    }

    if (results.size() > 12) results.resize(12);
    s_cache[ext] = results;
    return results;
}

// ---------------------------------------------------------------------------
// Game shortcut detection — parses .lnk files to identify game launchers
// ---------------------------------------------------------------------------

struct GameLaunchInfo {
    std::wstring platform;  // "Steam", "EA App", "Epic Games", "GOG Galaxy"
    std::wstring gameName;
    std::wstring launchExe;
    std::wstring launchArgs;
};

static std::wstring findArgToken(const std::wstring& args, const wchar_t* flag) {
    std::wstring lower = args, flagL(flag);
    for (auto& c : lower) c = towlower(c);
    for (auto& c : flagL) c = towlower(c);
    size_t pos = lower.find(flagL);
    if (pos == std::wstring::npos) return {};
    pos += flagL.size();
    while (pos < args.size() && iswspace(args[pos])) ++pos;
    size_t end = pos;
    while (end < args.size() && !iswspace(args[end])) ++end;
    return args.substr(pos, end - pos);
}

static std::wstring lookupSteamGameName(const std::wstring& appId) {
    HKEY h{};
    std::wstring key = L"Software\\Valve\\Steam\\Apps\\" + appId;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
        std::wstring name = readRegStr(h, L"Name");
        RegCloseKey(h);
        if (!name.empty()) return name;
    }
    key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App " + appId;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
        std::wstring name = readRegStr(h, L"DisplayName");
        RegCloseKey(h);
        if (!name.empty()) return name;
    }
    return {};
}

static std::wstring findSteamExe() {
    HKEY h{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        std::wstring p = readRegStr(h, L"SteamExe");
        RegCloseKey(h);
        if (!p.empty() && pathExists(p.c_str())) return p;
    }
    std::wstring pf = getKnownFolder(FOLDERID_ProgramFilesX86);
    if (!pf.empty()) {
        std::wstring p = pf + L"\\Steam\\steam.exe";
        if (pathExists(p.c_str())) return p;
    }
    return findOnPath(L"steam.exe");
}

static std::wstring lookupEAGameName(const std::wstring& contentId) {
    // EA stores game info under HKLM\SOFTWARE\EA Games\<game>
    HKEY hRoot{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EA Games", 0, KEY_READ, &hRoot) != ERROR_SUCCESS)
        return {};
    wchar_t subKey[256]; DWORD subLen = ARRAYSIZE(subKey);
    for (DWORD idx = 0; RegEnumKeyExW(hRoot, idx, subKey, &subLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; ++idx, subLen = ARRAYSIZE(subKey)) {
        HKEY hGame{};
        if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hGame) == ERROR_SUCCESS) {
            std::wstring id = readRegStr(hGame, L"ContentID");
            if (id == contentId) {
                std::wstring name = readRegStr(hGame, L"DisplayName");
                RegCloseKey(hGame); RegCloseKey(hRoot);
                return name;
            }
            RegCloseKey(hGame);
        }
    }
    RegCloseKey(hRoot);
    return {};
}

static std::optional<GameLaunchInfo> parseGameShortcut(const std::wstring& lnkPath) {
    IShellLinkW* psl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl)))
        return std::nullopt;

    IPersistFile* ppf = nullptr;
    if (FAILED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) { psl->Release(); return std::nullopt; }

    HRESULT hr = ppf->Load(lnkPath.c_str(), STGM_READ);
    ppf->Release();
    if (FAILED(hr)) { psl->Release(); return std::nullopt; }

    wchar_t target[MAX_PATH]{}, args[2048]{};
    psl->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH);
    psl->GetArguments(args, (int)ARRAYSIZE(args));

    std::wstring t(target), a(args);

    // For URL-type shortcuts (e.g. steam://rungameid/…), GetPath returns empty.
    // Fall back to IUniformResourceLocatorW which reads the URL target directly.
    if (t.empty()) {
        IUniformResourceLocatorW* pUrl = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_IUniformResourceLocatorW, (void**)&pUrl))) {
            wchar_t* url = nullptr;
            if (SUCCEEDED(pUrl->GetURL(&url)) && url) {
                t = url;
                CoTaskMemFree(url);
            }
            pUrl->Release();
        }
    }

    psl->Release();
    std::wstring base = exeBaseName(t);
    for (auto& c : base) c = towlower(c);

    // --- Steam ---
    // Shortcut formats:
    //   steam.exe -applaunch <id>              (old style)
    //   steam.exe steam://rungameid/<id>       (modern Steam shortcut)
    //   target IS steam://rungameid/<id>       (URL-type .lnk)
    auto extractRunGameId = [](const std::wstring& s) -> std::wstring {
        static const wchar_t* prefix = L"steam://rungameid/";
        size_t pos = s.find(prefix);
        if (pos == std::wstring::npos) return {};
        std::wstring id = s.substr(pos + wcslen(prefix));
        if (auto sp = id.find_first_of(L" \t/"); sp != std::wstring::npos) id.erase(sp);
        return id;
    };

    if (base == L"steam" || t.find(L"steam://") != std::wstring::npos) {
        std::wstring appId = findArgToken(a, L"-applaunch");
        if (appId.empty()) appId = extractRunGameId(a);
        if (appId.empty()) appId = extractRunGameId(t);
        if (!appId.empty()) {
            std::wstring steamExe = t.find(L"steam://") != std::wstring::npos ? findSteamExe() : t;
            std::wstring name = lookupSteamGameName(appId);
            return GameLaunchInfo{ L"Steam", name.empty() ? L"Game " + appId : name, steamExe, L"-applaunch " + appId };
        }
    }

    // --- EA App ---
    if (base == L"eadesktop" || base == L"origin" ||
        t.starts_with(L"eadesktop://") || t.starts_with(L"origin://launchgame/")) {
        // Extract content ID from eadesktop://launchgame/<id> or origin://launchgame/<id>
        std::wstring url = t.starts_with(L"http") ? a : t;
        std::wstring contentId;
        for (const wchar_t* prefix : { L"eadesktop://launchgame/", L"origin://launchgame/" }) {
            if (url.starts_with(prefix)) { contentId = url.substr(wcslen(prefix)); break; }
        }
        std::wstring name = contentId.empty() ? L"" : lookupEAGameName(contentId);
        return GameLaunchInfo{ L"EA App", name.empty() ? L"EA Game" : name, t, a };
    }

    // --- Epic Games ---
    if (base == L"epicgameslauncher" || t.starts_with(L"com.epicgames.launcher://")) {
        // Epic stores game manifests in %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests
        // but parsing JSON is heavy — use the shortcut description as the name
        wchar_t desc[1024]{};
        // Re-open just for description (psl is already released; re-load if needed — skip for now)
        return GameLaunchInfo{ L"Epic Games", L"Epic Game", t, a };
    }

    // --- GOG Galaxy ---
    if (base == L"gogalaxy" || t.starts_with(L"goggalaxy://openGame/")) {
        std::wstring gameId;
        if (t.starts_with(L"goggalaxy://openGame/")) gameId = t.substr(wcslen(L"goggalaxy://openGame/"));
        std::wstring name;
        if (!gameId.empty()) {
            HKEY h{};
            std::wstring key = L"SOFTWARE\\GOG.com\\Games\\" + gameId;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
                name = readRegStr(h, L"GAMENAME");
                RegCloseKey(h);
            }
        }
        return GameLaunchInfo{ L"GOG Galaxy", name.empty() ? L"GOG Game" : name, t, a };
    }

    return std::nullopt;
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
    m_flyouts.clear();
    m_activeFlyouts.clear();
    try {
        loadRegistryFilesAsSeparateFlyouts();
    } catch (...) {
        logDebug(L"Exception in loadRegistryFilesAsSeparateFlyouts", LogCategory::Error);
    }
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

    if (snapshot.selection.size() == 1 && PathIsDirectoryW(snapshot.selection.front().c_str())) {
        // Right-click on a folder icon: use the folder itself as context dir, not its parent
        snapshot.contextDir = snapshot.selection.front();
    } else if (snapshot.contextDir.empty() && !snapshot.selection.empty()) {
        // No pidlFolder: derive context dir from parent of first selection
        const std::wstring& firstPath = snapshot.selection.front();
        if (firstPath.length() < MAX_PATH - 1) {
            wchar_t dir[MAX_PATH] = {0};
            if (wcscpy_s(dir, ARRAYSIZE(dir), firstPath.c_str()) == 0) {
                PathRemoveFileSpecW(dir);
                snapshot.contextDir.assign(dir);
            }
        }
    }

    // Extract lowercase extension for single file selections (not directories)
    if (snapshot.selection.size() == 1 && !PathIsDirectoryW(snapshot.selection.front().c_str())) {
        const std::wstring& path = snapshot.selection.front();
        size_t dotPos = path.rfind(L'.');
        if (dotPos != std::wstring::npos && dotPos + 1 < path.size()) {
            snapshot.selectedExt = path.substr(dotPos + 1);
            for (auto& ch : snapshot.selectedExt) ch = towlower(ch);
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

Flyout AwesomeMenuHost::createContextAwareAwesomeMenu(const ContextSnapshot& snapshot) {
    logDebug(L"Creating context-aware AwesomeMenu", LogCategory::Menu);

    Flyout menu{};
    menu.name = L"AwesomeMenu";
    menu.label = L"Awesome Menu";

    ContextKind kind = snapshot.kind;
    ToolPaths tools = detectTools();

    // Lambda to add a FlyoutItem to any target flyout
    auto addItem = [](Flyout& target, std::wstring label, std::wstring cmd,
                      std::wstring args = L"", std::wstring icon = L"",
                      bool runAs = false, std::wstring section = L"") {
        FlyoutItem item{};
        item.label      = std::move(label);
        item.command    = std::move(cmd);
        item.args       = std::move(args);
        item.icon       = std::move(icon);
        item.workingDir = L"%DIR%";
        item.runAs      = runAs;
        item.section    = std::move(section);
        target.items.push_back(std::move(item));
    };

    bool isFolderCtx = (kind == ContextKind::Background   ||
                        kind == ContextKind::Directory     ||
                        kind == ContextKind::DesktopLocation   ||
                        kind == ContextKind::DocumentsLocation ||
                        kind == ContextKind::SystemLocation    ||
                        kind == ContextKind::ProjectLocation   ||
                        kind == ContextKind::HardDrive         ||
                        kind == ContextKind::RemovableDrive    ||
                        kind == ContextKind::NetworkDrive      ||
                        kind == ContextKind::OpticalDrive);

    if (isFolderCtx || kind == ContextKind::Multi) {
        // === Terminals ===
        if (!tools.wt.empty())
            addItem(menu, L"Windows Terminal Here", tools.wt,          L"-d \"%DIR%\"", tools.wt,          false, L"Terminals");
        if (!tools.pwsh.empty())
            addItem(menu, L"PowerShell 7 Here",     tools.pwsh,        L"",             tools.pwsh,        false, L"Terminals");
        addItem(menu,     L"PowerShell Here",        L"powershell.exe", L"",             L"powershell.exe", false, L"Terminals");
        addItem(menu,     L"Command Prompt Here",    L"cmd.exe",        L"",             L"cmd.exe",        false, L"Terminals");

        // === IDEs / Editors ===
        if (!tools.codeInsiders.empty())
            addItem(menu, L"Open with VS Code Insiders",  tools.codeInsiders, L"\"%DIR%\"", tools.codeInsiders, false, L"Editors");
        if (!tools.code.empty())
            addItem(menu, L"Open with VS Code",           tools.code,         L"\"%DIR%\"", tools.code,         false, L"Editors");
        if (!tools.cursor.empty())
            addItem(menu, L"Open with Cursor",            tools.cursor,       L"\"%DIR%\"", tools.cursor,       false, L"Editors");
        if (!tools.zed.empty())
            addItem(menu, L"Open with Zed",               tools.zed,          L"\"%DIR%\"", tools.zed,          false, L"Editors");
        if (!tools.sublimeText.empty())
            addItem(menu, L"Open with Sublime Text",      tools.sublimeText,  L"\"%DIR%\"", tools.sublimeText,  false, L"Editors");
        if (!tools.devenv.empty())
            addItem(menu, L"Open with Visual Studio 2022", tools.devenv,      L"\"%DIR%\"", tools.devenv,       false, L"Editors");

        // === Git submenu ===
        if (!tools.git.empty() || !tools.gitBash.empty()) {
            Flyout gitMenu{};
            gitMenu.name  = L"Git";
            gitMenu.label = L"Git";
            if (!tools.git.empty())
                addItem(gitMenu, L"Git GUI Here",  tools.git,     L"gui", tools.git);
            if (!tools.gitBash.empty())
                addItem(gitMenu, L"Git Bash Here", tools.gitBash, L"",    tools.gitBash);
            menu.subFlyouts.push_back(std::move(gitMenu));
        }

        // === VS 2022 Dev Shells submenu ===
        if (!tools.vs2022Root.empty()) {
            std::wstring vcvarsall   = tools.vs2022Root + L"\\VC\\Auxiliary\\Build\\vcvarsall.bat";
            std::wstring devShellDll = tools.vs2022Root + L"\\Common7\\Tools\\Microsoft.VisualStudio.DevShell.dll";

            if (pathExists(vcvarsall.c_str())) {
                Flyout devShells{};
                devShells.name  = L"DevShells";
                devShells.label = L"VS 2022 Dev Shells";

                FlyoutItem x64cmd{};
                x64cmd.label      = L"x64 Native Tools CMD";
                x64cmd.command    = L"cmd.exe";
                x64cmd.args       = L"/k \"\"" + tools.vs2022Root + L"\\VC\\Auxiliary\\Build\\vcvars64.bat\"\"";
                x64cmd.workingDir = L"%DIR%";
                x64cmd.icon       = L"cmd.exe";
                devShells.items.push_back(std::move(x64cmd));

                FlyoutItem x86cmd{};
                x86cmd.label      = L"x86 Native Tools CMD";
                x86cmd.command    = L"cmd.exe";
                x86cmd.args       = L"/k \"\"" + tools.vs2022Root + L"\\VC\\Auxiliary\\Build\\vcvars32.bat\"\"";
                x86cmd.workingDir = L"%DIR%";
                x86cmd.icon       = L"cmd.exe";
                devShells.items.push_back(std::move(x86cmd));

                if (pathExists(devShellDll.c_str())) {
                    FlyoutItem x64ps{};
                    x64ps.label      = L"x64 Native Tools PowerShell";
                    x64ps.command    = L"powershell.exe";
                    x64ps.args       = L"-NoExit -Command \"& { Import-Module '" + devShellDll +
                                       L"'; Enter-VsDevShell -VsInstallPath '" + tools.vs2022Root +
                                       L"' -DevCmdArguments '-arch=x64' -SkipAutomaticLocation }\"";
                    x64ps.workingDir = L"%DIR%";
                    x64ps.icon       = L"powershell.exe";
                    devShells.items.push_back(std::move(x64ps));

                    FlyoutItem x86ps{};
                    x86ps.label      = L"x86 Native Tools PowerShell";
                    x86ps.command    = L"powershell.exe";
                    x86ps.args       = L"-NoExit -Command \"& { Import-Module '" + devShellDll +
                                       L"'; Enter-VsDevShell -VsInstallPath '" + tools.vs2022Root +
                                       L"' -DevCmdArguments '-arch=x86' -SkipAutomaticLocation }\"";
                    x86ps.workingDir = L"%DIR%";
                    x86ps.icon       = L"powershell.exe";
                    devShells.items.push_back(std::move(x86ps));
                }

                menu.subFlyouts.push_back(std::move(devShells));
            }
        }

        // === JetBrains IDEs submenu (if any are installed) ===
        {
            bool anyJb = !tools.pycharm.empty() || !tools.rider.empty() ||
                         !tools.webStorm.empty() || !tools.clion.empty();
            if (anyJb) {
                Flyout jbMenu{};
                jbMenu.name  = L"JetBrains";
                jbMenu.label = L"JetBrains IDEs";
                if (!tools.pycharm.empty())   addItem(jbMenu, L"Open with PyCharm",   tools.pycharm,   L"\"%DIR%\"");
                if (!tools.rider.empty())     addItem(jbMenu, L"Open with Rider",     tools.rider,     L"\"%DIR%\"");
                if (!tools.webStorm.empty())  addItem(jbMenu, L"Open with WebStorm",  tools.webStorm,  L"\"%DIR%\"");
                if (!tools.clion.empty())     addItem(jbMenu, L"Open with CLion",     tools.clion,     L"\"%DIR%\"");
                menu.subFlyouts.push_back(std::move(jbMenu));
            }
        }

        // === As Admin submenu ===
        {
            Flyout asAdmin{};
            asAdmin.name  = L"AsAdmin";
            asAdmin.label = L"As Admin";
            if (!tools.wt.empty())
                addItem(asAdmin, L"Windows Terminal", tools.wt,          L"-d \"%DIR%\"", tools.wt,          true);
            if (!tools.pwsh.empty())
                addItem(asAdmin, L"PowerShell 7",     tools.pwsh,        L"",             tools.pwsh,        true);
            addItem(asAdmin,     L"PowerShell",        L"powershell.exe", L"",             L"powershell.exe", true);
            addItem(asAdmin,     L"Command Prompt",    L"cmd.exe",        L"",             L"cmd.exe",        true);
            menu.subFlyouts.push_back(std::move(asAdmin));
        }

    } else {
        // === File contexts — dynamically built from Windows file associations ===
        const std::wstring& ext = snapshot.selectedExt;

        // Query HKCR for all apps registered to open this extension
        auto openWith = queryOpenWith(ext);

        bool hasNotepad = false, hasHxD = false;

        for (const auto& app : openWith) {
            addItem(menu, L"Open with " + app.displayName, app.exePath, L"\"%SEL%\"", app.exePath);
            std::wstring b = exeBaseName(app.exePath);
            if (_wcsicmp(b.c_str(), L"notepad") == 0) hasNotepad = true;
            if (_wcsicmp(b.c_str(), L"HxD")     == 0) hasHxD     = true;
        }

        if (!hasNotepad) addItem(menu, L"Open with Notepad", L"notepad.exe", L"\"%SEL%\"");

        // HxD can open any file as hex — add if installed and not already listed
        if (!hasHxD && !tools.hxd.empty())
            addItem(menu, L"Open with HxD", tools.hxd, L"\"%SEL%\"", tools.hxd);

        // Game shortcuts — parse .lnk to identify Steam / EA / Epic / GOG launches
        if (ext == L"lnk" && snapshot.hasSelection()) {
            if (auto game = parseGameShortcut(snapshot.primarySelection())) {
                addItem(menu, L"Launch " + game->gameName + L" on " + game->platform,
                        game->launchExe, game->launchArgs, game->launchExe, false, L"Launch");
            }
        }

        // Extension-specific workflow actions (beyond the Open With registry)
        if (ext == L"ps1") {
            addItem(menu, L"Run in PowerShell",          L"powershell.exe",
                    L"-ExecutionPolicy Bypass -File \"%SEL%\"", L"powershell.exe", false, L"Run");
            addItem(menu, L"Run as Admin in PowerShell", L"powershell.exe",
                    L"-ExecutionPolicy Bypass -File \"%SEL%\"", L"powershell.exe", true,  L"Run");
        } else if (ext == L"bat" || ext == L"cmd") {
            addItem(menu, L"Run in CMD",          L"cmd.exe", L"/c \"%SEL%\"", L"cmd.exe", false, L"Run");
            addItem(menu, L"Run as Admin in CMD", L"cmd.exe", L"/c \"%SEL%\"", L"cmd.exe", true,  L"Run");
        } else if (ext == L"exe" || ext == L"com" || ext == L"scr") {
            addItem(menu, L"Run",          L"%SEL%", L"", L"", false, L"Run");
            addItem(menu, L"Run as Admin", L"%SEL%", L"", L"", true,  L"Run");
        } else if (ext == L"zip" || ext == L"rar" || ext == L"7z"  || ext == L"tar" ||
                   ext == L"gz"  || ext == L"bz2" || ext == L"xz"  || ext == L"cab") {
            if (!tools.sevenZip.empty()) {
                addItem(menu, L"Extract Here (7-Zip)",
                        tools.sevenZip, L"x \"%SEL%\" -o\"%DIR%\"",       tools.sevenZip, false, L"Extract");
                addItem(menu, L"Extract to Subfolder (7-Zip)",
                        tools.sevenZip, L"x \"%SEL%\" -o\"%DIR%\\*\" -y", tools.sevenZip, false, L"Extract");
            }
        }

        // As Admin submenu
        {
            Flyout asAdmin{};
            asAdmin.name  = L"AsAdmin";
            asAdmin.label = L"As Admin";
            if (!tools.wt.empty())
                addItem(asAdmin, L"Windows Terminal Here", tools.wt,          L"-d \"%DIR%\"", tools.wt,          true);
            addItem(asAdmin,     L"Command Prompt Here",   L"cmd.exe",        L"",             L"cmd.exe",        true);
            addItem(asAdmin,     L"PowerShell Here",       L"powershell.exe", L"",             L"powershell.exe", true);
            menu.subFlyouts.push_back(std::move(asAdmin));
        }
    }

    auto summary = std::format(L"Context-aware menu built for kind {} with {} items and {} submenus",
                               static_cast<int>(kind), menu.items.size(), menu.subFlyouts.size());
    logDebug(summary, LogCategory::Menu);

    return menu;
}

UINT AwesomeMenuHost::buildContextMenu(const ContextSnapshot& snapshot, HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT uFlags) {
    UNREFERENCED_PARAMETER(uFlags);

    std::vector<Flyout> activeFlyouts = m_flyouts;
    Flyout contextAware = createContextAwareAwesomeMenu(snapshot);
    activeFlyouts.insert(activeFlyouts.begin(), std::move(contextAware));
    m_activeFlyouts = std::move(activeFlyouts);

    UINT idNext = idCmdFirst;
    m_idCmdFirst = idCmdFirst;
    m_idToPath.clear();

    auto containsCase = [](const std::wstring& hay, const wchar_t* needle) {
        return StrStrIW(hay.c_str(), needle) != nullptr;
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

    UINT insertAt = indexMenu;
    UINT numInserted = 0;
    for (size_t f = 0; f < m_activeFlyouts.size(); ++f) {
        const auto& fly = m_activeFlyouts[f];
        if (!matchShowIn(fly.showIn)) continue;

        std::vector<UINT> rootPath = { static_cast<UINT>(f) };
        buildCascadingMenuFixed(hMenu, fly, idNext, rootPath, idCmdFirst, insertAt);
        ++insertAt;
        ++numInserted;
    }

    // Closing separator creates our own NVIDIA-style zone
    if (numInserted > 0) {
        MENUITEMINFOW sep{};
        sep.cbSize = sizeof(sep);
        sep.fMask = MIIM_FTYPE;
        sep.fType = MFT_SEPARATOR;
        InsertMenuItemW(hMenu, insertAt, TRUE, &sep);
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
    std::wstring p = getKnownFolder(FOLDERID_RoamingAppData);
    return p.empty() ? L"" : p + L"\\AwesomeMenuHost\\menus";
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
        if (!line.empty() && line[0] == 0xFEFF) line.erase(line.begin());
        trimInPlace(line);
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

    for (const auto& [key, item] : pendingItems) {
        if (!item.label.empty() && !item.command.empty())
            targetFlyout.items.push_back(item);
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
    expandPlaceholders(exe);                  // Replace %SEL% in command (e.g. run selected exe)
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

        // Guard against double-insertion: Explorer builds two separate HMENUs for
        // .lnk files (one for the shortcut, one for the resolved target) and calls
        // our handler on each. Track the last HMENU+tick; skip if same menu reappears
        // within 2 seconds (the resolved-target call always arrives within milliseconds).
        static HMENU s_dedupMenu = nullptr;
        static DWORD s_dedupTick = 0;
        DWORD now = GetTickCount();
        if (hMenu == s_dedupMenu && now - s_dedupTick < 2000)
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        s_dedupMenu = hMenu;
        s_dedupTick = now;

        ContextSnapshot snapshot = m_context;
        snapshot.kind = detectContextKind(snapshot);

        // Find position right after the first separator (Zone 1 / Zone 2 boundary) so
        // AwesomeMenu always anchors just below the top divider regardless of context.
        int count = GetMenuItemCount(hMenu);
        UINT insertPos = (UINT)count; // default: append if no separator found
        for (int i = 0; i < count; i++) {
            MENUITEMINFOW mii{};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_FTYPE;
            if (GetMenuItemInfoW(hMenu, i, TRUE, &mii) && (mii.fType & MFT_SEPARATOR)) {
                insertPos = (UINT)(i + 1);
                break;
            }
        }

        UINT used = buildContextMenu(snapshot, hMenu, insertPos, idCmdFirst, uFlags);
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
        if (!pici) return E_INVALIDARG;

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
            return E_FAIL; // string verbs not implemented
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
UINT AwesomeMenuHost::buildCascadingMenuFixed(HMENU hParentMenu, const Flyout& flyout, UINT& idNext, std::vector<UINT>& currentPath, UINT idCmdFirst, UINT insertAt) {
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

        if (!item.icon.empty()) {
            HBITMAP hIcon = hbitmapFromIconSpec(item.icon, GetSystemMetrics(SM_CXSMICON));
            if (hIcon) { mi.fMask |= MIIM_BITMAP; mi.hbmpItem = hIcon; m_menuBitmaps.push_back(hIcon); }
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

    InsertMenuItemW(hParentMenu, insertAt == UINT_MAX ? (UINT)GetMenuItemCount(hParentMenu) : insertAt, TRUE, &root);

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

    // Resolve bare exe names (e.g. "cmd.exe") to full system path so SHGetFileInfoW finds them
    std::wstring resolved = spec;
    if (spec.find(L'\\') == std::wstring::npos) {
        wchar_t full[MAX_PATH]{};
        if (SearchPathW(nullptr, spec.c_str(), nullptr, MAX_PATH, full, nullptr))
            resolved = full;
    }

    SHFILEINFOW sfi{};
    HICON hIcon = SHGetFileInfoW(resolved.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)
                  ? sfi.hIcon : nullptr;
    if (!hIcon)
        hIcon = (HICON)LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, sizePx, sizePx, LR_SHARED);
    if (!hIcon) return nullptr;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih); bih.biWidth = sizePx; bih.biHeight = -sizePx;
    bih.biPlanes = 1; bih.biBitCount = 32; bih.biCompression = BI_RGB;
    void* pvBits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hBmp = CreateDIBSection(hdc, (BITMAPINFO*)&bih, DIB_RGB_COLORS, &pvBits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hBmp) { DestroyIcon(hIcon); return nullptr; }

    ZeroMemory(pvBits, sizePx * sizePx * 4);
    HDC hdcMem = CreateCompatibleDC(nullptr);
    auto hOld = (HBITMAP)SelectObject(hdcMem, hBmp);
    DrawIconEx(hdcMem, 0, 0, hIcon, sizePx, sizePx, 0, nullptr, DI_NORMAL);
    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    DestroyIcon(hIcon);

    // Old-format icons (no 32-bit alpha channel) leave alpha=0 after DrawIconEx,
    // making the bitmap fully transparent. Detect this and force alpha=255.
    auto* px = static_cast<DWORD*>(pvBits);
    int n = sizePx * sizePx;
    bool has32Alpha = false;
    for (int i = 0; i < n && !has32Alpha; ++i)
        has32Alpha = (px[i] >> 24) != 0;
    if (!has32Alpha)
        for (int i = 0; i < n; ++i)
            px[i] |= 0xFF000000;

    return hBmp;
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