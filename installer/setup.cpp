// Setup ohne Admin, T41/T87
// Exe = Stub + angehängtes ZIP (make_setup.py):
//   [setup-stub.exe][paket.zip]["DIVAPKG1"][Größe des Pakets, 8 Byte LE]
// Ziel %LOCALAPPDATA%\DIVA PSP Recomp; games\ saves\ config\ work\ bleiben, der Rest wird ersetzt. Entpackt mit tar.exe
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objidl.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "launcher_res.h"

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace fs = std::filesystem;

namespace {

#define DIVA_W2(x) L##x
#define DIVA_W(x) DIVA_W2(x)
const wchar_t* const VERSION = DIVA_W(DIVA_VERSION_TEXT);
const wchar_t* const UNINSTALL_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DIVA PSP Recomp";

bool g_en = false;
const wchar_t* T(const wchar_t* de, const wchar_t* en) { return g_en ? en : de; }

HWND g_wnd, g_text, g_status, g_progress, g_ck_desktop, g_btn_ok, g_btn_cancel;
HFONT g_font, g_font_big;
std::atomic<bool> g_running{false};
bool g_done = false;
const UINT WM_STATUS = WM_APP + 1, WM_FINISHED = WM_APP + 2;
enum { ID_OK = 100, ID_CANCEL, ID_DESKTOP };

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
fs::path install_dir() { return known_folder(FOLDERID_LocalAppData) / L"DIVA PSP Recomp"; }
fs::path self_path() {
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    return b;
}

void post_status(const std::wstring& s) { if (g_wnd) PostMessageW(g_wnd, WM_STATUS, 0, (LPARAM) new std::wstring(s)); }

// Paket in der eigenen Exe: Offset, Größe (0 = keins)
std::pair<uint64_t, uint64_t> find_payload() {
    std::ifstream f(self_path(), std::ios::binary | std::ios::ate);
    const uint64_t end = (uint64_t)f.tellg();
    if (end < 16) return {0, 0};
    f.seekg((std::streamoff)(end - 16));
    char magic[8];
    uint64_t size = 0;
    f.read(magic, 8);
    f.read((char*)&size, 8);
    if (!f || memcmp(magic, "DIVAPKG1", 8) != 0 || size == 0 || size > end - 16) return {0, 0};
    return {end - 16 - size, size};
}

int run_hidden(std::wstring cl, const fs::path& cwd) {
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

bool make_link(const fs::path& link, const fs::path& target) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(target.parent_path().c_str());
    sl->SetDescription(L"DIVA PSP Recomp Launcher");
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(link.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    return ok;
}

void reg_set(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void reg_set(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v); }

uint64_t dir_size(const fs::path& p) {
    std::error_code ec;
    uint64_t n = 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    return n;
}

// Installation (Thread); leer = ok
std::wstring install(bool desktop) {
    std::error_code ec;
    const auto [off, size] = find_payload();
    if (!size) return T(L"Das Installationspaket fehlt in dieser Datei (beschädigter Download?).", L"The installation package is missing from this file (damaged download?).");
    const fs::path dir = install_dir();
    fs::create_directories(dir, ec);
    if (ec) return T(L"Der Ordner konnte nicht angelegt werden:\n", L"The folder could not be created:\n") + dir.wstring();
    // alte Programmdateien weg; gesperrt = Launcher läuft noch
    post_status(T(L"Alte Programmdateien entfernen …", L"Removing old program files …"));
    fs::remove(dir / L"DIVA Launcher.exe", ec);
    if (ec && fs::exists(dir / L"DIVA Launcher.exe")) return T(L"Der Launcher läuft noch. Bitte schließe ihn und starte das Setup erneut.", L"The launcher is still running. Please close it and start the setup again.");
    for (const wchar_t* d : {L"bin", L"src", L"tools"}) {
        fs::remove_all(dir / d, ec);
        if (ec) return T(L"Alte Dateien konnten nicht gelöscht werden (läuft das Spiel noch?):\n", L"Old files could not be deleted (is the game still running?):\n") + (dir / d).wstring();
    }
    // Paket kopieren + entpacken (kurze ASCII-Namen für tar.exe)
    const fs::path zip = dir / L"setup-package.zip";
    {
        std::ifstream in(self_path(), std::ios::binary);
        std::ofstream out(zip, std::ios::binary);
        in.seekg((std::streamoff)off);
        std::vector<char> b(4 << 20);
        uint64_t left = size;
        int last = -1;
        while (left && in && out) {
            const size_t n = (size_t)std::min<uint64_t>(left, b.size());
            in.read(b.data(), (std::streamsize)n);
            out.write(b.data(), (std::streamsize)n);
            left -= n;
            const int pct = (int)((size - left) * 100 / size);
            if (pct != last) {
                last = pct;
                post_status(T(L"Paket kopieren … ", L"Copying package … ") + std::to_wstring(pct) + L" %");
            }
        }
        if (left || !out) { out.close(); fs::remove(zip, ec); return T(L"Nicht genug Speicherplatz?", L"Not enough disk space?"); }
    }
    post_status(T(L"Dateien entpacken (dauert ein bis zwei Minuten) …", L"Unpacking files (takes a minute or two) …"));
    const int rc = run_hidden(L"\"" + (known_folder(FOLDERID_System) / L"tar.exe").wstring() + L"\" -x -f setup-package.zip", dir);
    fs::remove(zip, ec);
    if (rc != 0 || !fs::exists(dir / L"DIVA Launcher.exe", ec))
        return T(L"Das Entpacken ist fehlgeschlagen (tar.exe, Code ", L"Unpacking failed (tar.exe, code ") + std::to_wstring(rc) +
               T(L"). Windows 10 Version 1803 oder neuer wird benötigt.", L"). Windows 10 version 1803 or newer is required.");
    // Links + Apps-Eintrag
    post_status(T(L"Verknüpfungen anlegen …", L"Creating shortcuts …"));
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const fs::path exe = dir / L"DIVA Launcher.exe";
    make_link(known_folder(FOLDERID_Programs) / L"DIVA PSP Recomp.lnk", exe);
    if (desktop) make_link(known_folder(FOLDERID_Desktop) / L"DIVA PSP Recomp.lnk", exe);
    CoUninitialize();
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        reg_set(k, L"DisplayName", L"DIVA PSP Recomp");
        reg_set(k, L"DisplayVersion", VERSION);
        reg_set(k, L"Publisher", L"Simulant235");
        reg_set(k, L"DisplayIcon", exe.wstring() + L",0");
        reg_set(k, L"InstallLocation", dir.wstring());
        reg_set(k, L"UninstallString", L"\"" + exe.wstring() + L"\" --uninstall");
        reg_set(k, L"NoModify", 1);
        reg_set(k, L"NoRepair", 1);
        reg_set(k, L"EstimatedSize", (DWORD)((dir_size(dir / L"bin") + dir_size(dir / L"src") + dir_size(dir / L"tools") + fs::file_size(exe, ec)) >> 10));
        RegCloseKey(k);
    }
    return {};
}

HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

void start_install() {
    if (g_running.exchange(true)) return;
    EnableWindow(g_btn_ok, FALSE);
    EnableWindow(g_btn_cancel, FALSE);
    EnableWindow(g_ck_desktop, FALSE);
    ShowWindow(g_progress, SW_SHOW);
    SendMessageW(g_progress, PBM_SETMARQUEE, TRUE, 30);
    const bool desktop = SendMessageW(g_ck_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::thread([desktop] { PostMessageW(g_wnd, WM_FINISHED, 0, (LPARAM) new std::wstring(install(desktop))); }).detach();
}

LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_COMMAND:
        if (LOWORD(wp) == ID_OK) {
            if (!g_done) start_install();
            else {   // "Launcher starten"
                ShellExecuteW(w, L"open", (install_dir() / L"DIVA Launcher.exe").c_str(), nullptr, install_dir().c_str(), SW_SHOWNORMAL);
                DestroyWindow(w);
            }
        } else if (LOWORD(wp) == ID_CANCEL) DestroyWindow(w);
        return 0;
    case WM_STATUS: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        SetWindowTextW(g_status, s->c_str());
        return 0;
    }
    case WM_FINISHED: {
        std::unique_ptr<std::wstring> err((std::wstring*)lp);
        g_running = false;
        ShowWindow(g_progress, SW_HIDE);
        if (err->empty()) {
            g_done = true;
            SetWindowTextW(g_status, T(L"Fertig! DIVA PSP Recomp ist installiert.", L"Done! DIVA PSP Recomp is installed."));
            SetWindowTextW(g_btn_ok, T(L"Launcher starten", L"Start launcher"));
            SetWindowTextW(g_btn_cancel, T(L"Schließen", L"Close"));
            EnableWindow(g_btn_ok, TRUE);
            EnableWindow(g_btn_cancel, TRUE);
            SetFocus(g_btn_ok);
        } else {
            SetWindowTextW(g_status, T(L"Installation fehlgeschlagen.", L"Installation failed."));
            MessageBoxW(w, err->c_str(), L"DIVA PSP Recomp Setup", MB_ICONERROR | MB_OK);
            EnableWindow(g_btn_ok, TRUE);
            EnableWindow(g_btn_cancel, TRUE);
            EnableWindow(g_ck_desktop, TRUE);
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_running) return 0;   // nicht schließen während der Installation
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_en = PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_GERMAN;
    {   // --silent [--no-desktop]: Exit-Code, Fehler nach stderr
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool silent = false, desktop = true;
        for (int i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--silent") == 0) silent = true;
            if (wcscmp(argv[i], L"--no-desktop") == 0) desktop = false;
        }
        LocalFree(argv);
        if (silent) {
            g_wnd = nullptr;
            const std::wstring err = install(desktop);
            if (!err.empty()) fwprintf(stderr, L"%ls\n", err.c_str());
            return err.empty() ? 0 : 1;
        }
    }
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"DivaSetup";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);
    RECT rc = {0, 0, 560, 330};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    const std::wstring title = L"DIVA PSP Recomp " + std::wstring(VERSION) + L" – Setup";
    g_wnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, inst, nullptr);
    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_big = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    mk(L"STATIC", (L"DIVA PSP Recomp " + std::wstring(VERSION)).c_str(), 0, 24, 18, 510, 34, 0, g_font_big);
    const std::wstring text =
        T(L"Installiert nach:\n", L"Installs to:\n") + install_dir().wstring() +
        T(L"\n\nKeine Admin-Rechte nötig, etwa 1 GB Speicherplatz. Spiele, Spielstände und Einstellungen einer früheren "
          L"Installation bleiben erhalten. Spieldateien sind nicht enthalten: Du brauchst deine eigene ISO.",
          L"\n\nNo admin rights needed, about 1 GB of disk space. Games, save data and settings of an earlier installation "
          L"are kept. No game files are included: you need your own ISO.");
    g_text = mk(L"STATIC", text.c_str(), 0, 24, 62, 510, 130, 0, g_font);
    g_ck_desktop = mk(L"BUTTON", T(L"Verknüpfung auf dem Desktop anlegen", L"Create a desktop shortcut"), BS_AUTOCHECKBOX | WS_TABSTOP, 24, 196,
                      400, 24, ID_DESKTOP, g_font);
    SendMessageW(g_ck_desktop, BM_SETCHECK, BST_CHECKED, 0);
    g_status = mk(L"STATIC", L"", 0, 24, 230, 510, 24, 0, g_font);
    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_MARQUEE, 24, 258, 510, 10, g_wnd, nullptr, inst, nullptr);
    g_btn_ok = mk(L"BUTTON", T(L"Installieren", L"Install"), BS_DEFPUSHBUTTON | WS_TABSTOP, 254, 280, 140, 34, ID_OK, g_font);
    g_btn_cancel = mk(L"BUTTON", T(L"Abbrechen", L"Cancel"), WS_TABSTOP, 404, 280, 130, 34, ID_CANCEL, g_font);
    if (!find_payload().second) {   // Stub ohne Paket
        SetWindowTextW(g_status, T(L"Kein Installationspaket in dieser Datei (nur der Setup-Rumpf).", L"No installation package in this file (setup stub only)."));
        EnableWindow(g_btn_ok, FALSE);
    }
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    SetFocus(g_btn_ok);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
