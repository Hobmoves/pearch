#include "platform/shell.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "core/fileio.h"
#include "core/text.h"

namespace shell {

namespace {

std::wstring W(const std::string& s) { return text::to_wide(s); }

std::string known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) out = text::to_utf8(p);
    CoTaskMemFree(p);
    return out;
}

bool set_reg(const std::wstring& subkey, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

bool get_user_path(std::wstring& value, DWORD& type) {
    HKEY k;
    value.clear();
    type = REG_EXPAND_SZ;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD size = 0;
    LONG r = RegQueryValueExW(k, L"Path", nullptr, &type, nullptr, &size);
    if (r == ERROR_SUCCESS && size > 0) {
        value.resize(size / sizeof(wchar_t));
        RegQueryValueExW(k, L"Path", nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size);
        while (!value.empty() && value.back() == L'\0') value.pop_back();
    }
    RegCloseKey(k);
    return true;
}

bool set_user_path(const std::wstring& value, DWORD type) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_WRITE, &k) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(k, L"Path", 0, type == REG_SZ ? REG_SZ : REG_EXPAND_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 3000, nullptr);
    return r == ERROR_SUCCESS;
}

std::vector<std::wstring> split_path(const std::wstring& v) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i <= v.size(); ++i) {
        if (i == v.size() || v[i] == L';') {
            if (i > start) out.push_back(v.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool same_path(const std::wstring& a, const std::wstring& b) {
    std::wstring x = a, y = b;
    while (!x.empty() && (x.back() == L'\\' || x.back() == L'/')) x.pop_back();
    while (!y.empty() && (y.back() == L'\\' || y.back() == L'/')) y.pop_back();
    return CompareStringOrdinal(x.c_str(), -1, y.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::string install_dir() { return fileio::join(known_folder(FOLDERID_LocalAppData), "Programs\\pearch"); }
std::string shortcut_path() { return fileio::join(known_folder(FOLDERID_Programs), "pearch.lnk"); }

const wchar_t* kMenuKeys[] = {
    L"Software\\Classes\\Directory\\Background\\shell\\pearch",
    L"Software\\Classes\\Directory\\shell\\pearch",
    L"Software\\Classes\\Drive\\shell\\pearch",
};

}  // namespace

void init() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
}

std::vector<std::string> utf8_args(int argc, char** argv) {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (w) {
        for (int i = 0; i < n; ++i) out.push_back(text::to_utf8(w[i]));
        LocalFree(w);
        return out;
    }
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

bool open(const std::string& path) {
    std::wstring w = W(path);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpFile = w.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

bool reveal(const std::string& path) {
    std::wstring w = W(path);
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(w.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
        if (SUCCEEDED(hr)) return true;
    }
    std::wstring args = L"/select,\"" + w + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                                                   SW_SHOWNORMAL)) > 32;
}

bool copy_text(const std::string& s) {
    std::wstring w = W(s);
    if (!OpenClipboard(GetConsoleWindow())) return false;
    EmptyClipboard();
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h) {
        void* p = GlobalLock(h);
        std::memcpy(p, w.c_str(), bytes);
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

std::string data_dir() {
    if (const wchar_t* env = _wgetenv(L"PEARCH_HOME"); env && *env) return text::to_utf8(env);
    std::string base = known_folder(FOLDERID_LocalAppData);
    if (base.empty()) base = fileio::absolute(".");
    return fileio::join(base, "pearch");
}

std::string home_dir() { return known_folder(FOLDERID_Profile); }

std::string exe_path() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return text::to_utf8(buf);
}

Defaults default_locations() {
    Defaults d;
    std::string home = home_dir();
    d.roots.push_back(home);
    // Libraries redirected outside the profile (another drive, a work OneDrive...).
    for (REFKNOWNFOLDERID id : {FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads,
                                FOLDERID_Pictures, FOLDERID_Music, FOLDERID_Videos}) {
        std::string p = known_folder(id);
        if (p.empty() || fileio::is_within(p, home)) continue;
        bool dup = false;
        for (const auto& r : d.roots) dup = dup || fileio::is_within(p, r);
        if (!dup && fileio::is_directory(p)) d.roots.push_back(p);
    }
    // Start Menu programs: what Windows search calls "Apps".
    for (REFKNOWNFOLDERID id : {FOLDERID_Programs, FOLDERID_CommonPrograms}) {
        std::string p = known_folder(id);
        if (p.empty()) continue;
        d.roots.push_back(p);
        d.app_paths.push_back(p);
    }
    d.skip_paths.push_back(fileio::join(home, "AppData"));
    return d;
}

std::vector<Step> install() {
    std::vector<Step> steps;
    const std::string dir = install_dir();
    const std::string target = fileio::join(dir, "pearch.exe");
    const std::string self = exe_path();
    fileio::make_dirs(dir);
    if (same_path(W(self), W(target))) {
        steps.push_back({true, "pearch is already in " + dir});
    } else if (CopyFileW(W(self).c_str(), W(target).c_str(), FALSE)) {
        steps.push_back({true, "Copied pearch.exe to " + dir});
    } else {
        steps.push_back({false, "Could not copy pearch.exe to " + dir + " (is it running?)"});
        return steps;
    }

    std::wstring path_value;
    DWORD type = REG_EXPAND_SZ;
    get_user_path(path_value, type);
    bool present = false;
    for (const auto& p : split_path(path_value)) present = present || same_path(p, W(dir));
    if (present) {
        steps.push_back({true, "Already on your PATH"});
    } else {
        if (!path_value.empty() && path_value.back() != L';') path_value += L';';
        path_value += W(dir);
        bool ok = set_user_path(path_value, type);
        steps.push_back({ok, ok ? "Added to your PATH (new terminals can run `pearch`)"
                                : "Could not update your PATH"});
    }

    bool link_ok = false;
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                   reinterpret_cast<void**>(&link)))) {
        std::wstring wt = W(target);
        link->SetPath(wt.c_str());
        link->SetDescription(L"pearch \x2014 instant file search");
        link->SetIconLocation(wt.c_str(), 0);
        link->SetWorkingDirectory(W(home_dir()).c_str());
        link->SetHotkey(MAKEWORD(VK_SPACE, HOTKEYF_CONTROL | HOTKEYF_ALT));
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
            link_ok = SUCCEEDED(file->Save(W(shortcut_path()).c_str(), TRUE));
            file->Release();
        }
        link->Release();
    }
    steps.push_back({link_ok, link_ok ? "Start Menu shortcut + global hotkey Ctrl+Alt+Space"
                                      : "Could not create the Start Menu shortcut"});

    const std::wstring exe = L"\"" + W(target) + L"\"";
    bool menu_ok = true;
    for (int i = 0; i < 3; ++i) {
        std::wstring key = kMenuKeys[i];
        std::wstring arg = i == 0 ? L"%V" : L"%1";
        menu_ok &= set_reg(key, nullptr, i == 0 ? L"Search here with pearch" : L"Search with pearch");
        menu_ok &= set_reg(key, L"Icon", exe + L",0");
        menu_ok &= set_reg(key + L"\\command", nullptr, exe + L" --in \"" + arg + L"\"");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    steps.push_back({menu_ok, menu_ok ? "Explorer right-click: \"Search here with pearch\""
                                      : "Could not add the Explorer menu entry"});
    return steps;
}

std::vector<Step> uninstall(bool purge) {
    std::vector<Step> steps;
    const std::string dir = install_dir();
    const std::string target = fileio::join(dir, "pearch.exe");

    if (fileio::exists(shortcut_path())) {
        bool ok = DeleteFileW(W(shortcut_path()).c_str()) != 0;
        steps.push_back({ok, ok ? "Removed the Start Menu shortcut and hotkey" : "Could not remove the shortcut"});
    }
    for (const wchar_t* key : kMenuKeys) RegDeleteTreeW(HKEY_CURRENT_USER, key);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    steps.push_back({true, "Removed the Explorer menu entry"});

    std::wstring path_value;
    DWORD type = REG_EXPAND_SZ;
    if (get_user_path(path_value, type)) {
        std::wstring kept;
        bool removed = false;
        for (const auto& p : split_path(path_value)) {
            if (same_path(p, W(dir))) { removed = true; continue; }
            if (!kept.empty()) kept += L';';
            kept += p;
        }
        if (removed) {
            set_user_path(kept, type);
            steps.push_back({true, "Removed from your PATH"});
        }
    }

    if (fileio::exists(target)) {
        if (same_path(W(exe_path()), W(target))) {
            // A running exe can't delete itself: let cmd do it a moment later.
            std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + W(target) +
                               L"\" & rmdir \"" + W(dir) + L"\"";
            STARTUPINFOW si{};
            si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                               nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        } else {
            DeleteFileW(W(target).c_str());
            RemoveDirectoryW(W(dir).c_str());
        }
        steps.push_back({true, "Removed " + dir});
    }

    if (purge) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(W(data_dir())), ec);
        steps.push_back({!ec, !ec ? "Deleted the index and settings" : "Could not delete " + data_dir()});
    }
    return steps;
}

}  // namespace shell
