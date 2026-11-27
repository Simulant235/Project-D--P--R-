// DIVA Launcher + Install-Wizard in einem (T41)
//   Kacheln 1st/2nd/Extend: ISO wählen oder reinziehen -> bauen -> Spielen
//   Einstellungen -> settings.ini (liest die Spiel-.exe), Fehler melden, Über/Credits, DE/EN, Updater
// gebaut wird beim Spieler (Spielcode darf nicht verteilt werden): EBOOT aus der ISO, diva_recomp, llvm-mingw + CMake,
// Exe + DLLs nach games\<spiel>\
// Ordner %LOCALAPPDATA%\DIVA PSP Recomp: bin\ src\ tools\ ersetzt das Update, config\ games\ saves\ work\ bleiben
// ohne Fenster: --install-test <iso>, --uninstall, --updated
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <atomic>
#include <cctype>
#include <memory>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "launcher_res.h"
#include "alpha_guard.h"

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- Sprache (Deutsch / Englisch)
// T(L"deutsch", L"english"); Wahl in config\launcher.ini (lang = de|en), erster Start nach Windows-Sprache
bool g_en = false;
const wchar_t* T(const wchar_t* de, const wchar_t* en) { return g_en ? en : de; }

#define DIVA_W2(x) L##x
#define DIVA_W(x) DIVA_W2(x)
const wchar_t* const VERSION = DIVA_W(DIVA_VERSION_TEXT);
const wchar_t* const UPDATE_REPO = DIVA_W(DIVA_UPDATE_REPO);   // release.cmake; leer = Updater aus
const wchar_t* const REPORT_TO = DIVA_W(DIVA_REPORT_TO);       // release.cmake; Ziel der Fehlerberichte

// ---------------------------------------------------------------- Spiele
struct Game {
    const wchar_t* key;      // Kurzname, Ordner unter games/ und work/
    const wchar_t* title;
    const char* disc;        // Spiel-ID aus UMD_DATA.BIN, ohne Bindestrich
    const wchar_t* toml;     // games\<toml> im Quellbaum
    const wchar_t* exe;      // Name der Spiel-.exe (CMake-Ziel)
    bool supported;
};
const Game GAMES[3] = {
    {L"1st", L"Project DIVA 1st", "ULJM05472", L"diva1st.toml", L"diva1st", true},
    {L"2nd", L"Project DIVA 2nd", "ULJM05681", L"diva2nd.toml", L"diva2nd", false},
    {L"extend", L"Project DIVA Extend", "ULJM05933", L"divaextend.toml", L"divaextend", false},
};

// ---------------------------------------------------------------- Pfade
struct Layout { fs::path root, data, src, tc, cmake, recomp, saves, cfg; } L;

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

fs::path search_exe(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    return SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr) ? fs::path(buf) : fs::path();
}

void find_layout() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    L.root = fs::path(buf).parent_path();
    std::error_code ec;
    // Quellbaum: src\ oder Entwickler-Checkout
    if (fs::exists(L.root / L"src" / L"runtime" / L"CMakeLists.txt", ec)) L.src = L.root / L"src";
    else
        for (fs::path p = L.root; p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
            if (fs::exists(p / L"runtime" / L"CMakeLists.txt", ec)) { L.src = p; break; }
    // llvm-mingw (mitgeliefert)
    if (fs::exists(L.root / L"tools" / L"llvm-mingw" / L"bin" / L"clang.exe", ec)) L.tc = L.root / L"tools" / L"llvm-mingw";
    else if (!L.src.empty() && fs::exists(L.src / L"tools", ec))
        for (auto& e : fs::directory_iterator(L.src / L"tools", ec))
            if (e.is_directory() && e.path().filename().wstring().rfind(L"llvm-mingw", 0) == 0 && fs::exists(e.path() / L"bin" / L"clang.exe", ec)) { L.tc = e.path(); break; }
    // CMake
    if (fs::exists(L.root / L"tools" / L"cmake" / L"bin" / L"cmake.exe", ec)) L.cmake = L.root / L"tools" / L"cmake" / L"bin" / L"cmake.exe";
    else L.cmake = search_exe(L"cmake.exe");
    // Recompiler
    for (fs::path c : {L.root / L"bin" / L"diva_recomp.exe", L.src / L"build" / L"recompiler" / L"Release" / L"diva_recomp.exe",
                       L.src / L"build-mingw" / L"recompiler" / L"diva_recomp.exe"})
        if (!L.src.empty() || c.is_absolute())
            if (fs::exists(c, ec)) { L.recomp = c; break; }
    L.data = known_folder(FOLDERID_LocalAppData) / L"DIVA PSP Recomp";
    L.saves = L.data / L"saves";
    L.cfg = L.data / L"config";    // launcher.ini, settings.ini
    fs::create_directories(L.data, ec);
    fs::create_directories(L.saves, ec);
    fs::create_directories(L.cfg, ec);
    for (const wchar_t* n : {L"launcher.ini", L"settings.ini"})   // Umzug alter Fassungen
        if (fs::exists(L.data / n, ec) && !fs::exists(L.cfg / n, ec)) fs::rename(L.data / n, L.cfg / n, ec);
    // alte Spielstände (Dokumente\DIVA PSP Recomp\saves) übernehmen
    const fs::path old_saves = known_folder(FOLDERID_Documents) / L"DIVA PSP Recomp" / L"saves";
    if (fs::is_empty(L.saves, ec) && fs::exists(old_saves, ec) && !fs::is_empty(old_saves, ec))
        fs::copy(old_saves, L.saves, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
}

// ---------------------------------------------------------------- kleine Schlüssel/Wert-Dateien
std::map<std::wstring, std::wstring> read_kv(const fs::path& p) {
    std::map<std::wstring, std::wstring> m;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        m[fs::path(std::u8string(k.begin(), k.end())).wstring()] = fs::path(std::u8string(v.begin(), v.end())).wstring();
    }
    return m;
}
std::string u8(const std::wstring& w) { const auto s = fs::path(w).u8string(); return std::string(s.begin(), s.end()); }

std::map<std::wstring, std::wstring> g_state;   // config\launcher.ini: iso_<spiel>, built_<spiel>, theme, lang, ...
void save_state() {
    std::ofstream f(L.cfg / L"launcher.ini");
    f << "# DIVA Launcher: Zustand (wird automatisch geschrieben)\n";
    for (auto& [k, v] : g_state) f << u8(k) << " = " << u8(v) << "\n";
}

// ---------------------------------------------------------------- ISO 9660 (nur lesen)
struct IsoFile { uint32_t lba = 0, size = 0; bool ok = false; };

IsoFile iso_find(FILE* f, const std::string& path) {
    auto rd = [&](uint32_t lba, std::vector<uint8_t>& b, uint32_t n) {
        b.resize(n);
        return _fseeki64(f, (long long)lba * 2048, SEEK_SET) == 0 && fread(b.data(), 1, n, f) == n;
    };
    std::vector<uint8_t> s;
    if (!rd(16, s, 2048) || memcmp(&s[1], "CD001", 5) != 0) return {};
    auto le = [](const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); };
    uint32_t lba = le(&s[156 + 2]), size = le(&s[156 + 10]);
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        std::vector<uint8_t> d;
        if (!rd(lba, d, (size + 2047) / 2048 * 2048)) return {};
        bool found = false;
        for (uint32_t o = 0; o < d.size();) {
            const uint8_t len = d[o];
            if (!len) { o = (o / 2048 + 1) * 2048; continue; }
            std::string name((const char*)&d[o + 33], d[o + 32]);
            if (const size_t sc = name.find(';'); sc != std::string::npos) name.resize(sc);
            if (_stricmp(name.c_str(), part.c_str()) == 0) { lba = le(&d[o + 2]); size = le(&d[o + 10]); found = true; break; }
            o += len;
        }
        if (!found) return {};
        if (slash == std::string::npos) return {lba, size, true};
        pos = slash + 1;
    }
    return {};
}

// Spiel-ID der ISO (z.B. "ULJM05472") oder leer
std::string iso_disc_id(const fs::path& iso) {
    FILE* f = _wfopen(iso.c_str(), L"rb");
    if (!f) return {};
    std::string id;
    const IsoFile u = iso_find(f, "UMD_DATA.BIN");
    if (u.ok) {
        char buf[32] = {};
        _fseeki64(f, (long long)u.lba * 2048, SEEK_SET);
        fread(buf, 1, std::min<uint32_t>(u.size, 31), f);
        for (char* p = buf; *p && *p != '|'; p++)
            if (*p != '-') id += *p;
    }
    fclose(f);
    return id;
}

bool iso_extract(const fs::path& iso, const std::string& path, const fs::path& out) {
    FILE* f = _wfopen(iso.c_str(), L"rb");
    if (!f) return false;
    const IsoFile e = iso_find(f, path);
    bool ok = false;
    if (e.ok) {
        std::vector<char> b(e.size);
        _fseeki64(f, (long long)e.lba * 2048, SEEK_SET);
        if (fread(b.data(), 1, b.size(), f) == b.size()) {
            std::ofstream o(out, std::ios::binary);
            o.write(b.data(), (std::streamsize)b.size());
            ok = (bool)o;
        }
    }
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------- Bilder (PNG aus den Ressourcen, GDI+)
Gdiplus::Bitmap* load_png(int id) {
    HRSRC rs = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!rs) return nullptr;
    const DWORD size = SizeofResource(nullptr, rs);
    const void* data = LockResource(LoadResource(nullptr, rs));
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    memcpy(GlobalLock(mem), data, size);
    GlobalUnlock(mem);
    IStream* st = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &st))) return nullptr;
    auto* bmp = Gdiplus::Bitmap::FromStream(st);
    st->Release();
    return bmp;
}
Gdiplus::Bitmap *g_img_game[3], *g_img_loading, *g_img_warning;          // aktuell benutzte Bilder
Gdiplus::Bitmap *g_img_dark[2][5];                                         // [0] schwarz (hell), [1] weiß (dunkel)

// ---------------------------------------------------------------- Design (hell/dunkel)
bool g_dark = false;
HBRUSH g_bg_brush = nullptr;
COLORREF g_bg = RGB(255, 255, 255), g_fg = RGB(0, 0, 0);

bool system_prefers_dark() {
    DWORD v = 1, size = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                        RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v == 0;
}

HWND g_wnd;   // Hauptfenster (für msg)

// Meldung mit eigenem Icon
int msg(const std::wstring& text, const wchar_t* title, int icon, UINT buttons = MB_OK) {
    MSGBOXPARAMSW mb = {sizeof mb};
    mb.hwndOwner = g_wnd && IsWindowVisible(g_wnd) ? g_wnd : GetActiveWindow();
    mb.hInstance = GetModuleHandleW(nullptr);
    mb.lpszText = text.c_str();
    mb.lpszCaption = title;
    mb.dwStyle = buttons | MB_USERICON;
    mb.lpszIcon = MAKEINTRESOURCEW(icon);
    return MessageBoxIndirectW(&mb);
}

// ---------------------------------------------------------------- Fenster
HWND g_title, g_status, g_progress;
HWND g_tile_name[3], g_tile_state[3], g_tile_btn[3];
HWND g_btn_settings, g_btn_saves, g_btn_report, g_btn_about;
std::vector<HWND> g_main_ctl, g_set_ctl;
HWND g_cb_renderer, g_cb_scale, g_cb_fps, g_ck_xbrz, g_cb_xbrz_lvl, g_ck_latency, g_ed_latency, g_btn_save, g_btn_back;
HWND g_btn_lat_reset;          // Zurücksetzen der festen Latenz
const wchar_t* g_lat_help = L"";   // Tooltip-Text zum „?“ (per Rückruf, kein Längenlimit)
const UINT_PTR TIP_LAT_HELP = 1;   // Tool-ID „?“
HWND g_tip;                    // Tooltips („?“, Zurücksetzen)
HFONT g_font_icon;             // Segoe MDL2 Assets
const int LATENCY_DEFAULT = 50;   // Standard der festen Latenz in ms
HWND g_btn_uninst_game, g_btn_del_saves, g_btn_uninst_all, g_cb_theme, g_cb_lang;
std::vector<HWND> g_test_ctl;
HWND g_test_status, g_ck_test[3];
void fill_uninstall_list();
bool g_xbrz_touched = false;   // xBRZ von Hand umgeschaltet
HFONT g_font, g_font_big, g_font_tile;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_unknown_ok{false};   // Bastler-Schalter, gilt für den laufenden Bau
HWND g_ck_unknown;
const UINT WM_STATUS = WM_APP + 1, WM_BUILD_DONE = WM_APP + 2, WM_GAME_FAILED = WM_APP + 3, WM_UPDATE_FOUND = WM_APP + 4,
           WM_UPDATE_DONE = WM_APP + 5, WM_GAME_CRASHED = WM_APP + 6;

fs::path game_dir(int i) { return L.data / L"games" / GAMES[i].key; }
fs::path game_exe(int i) { return game_dir(i) / (std::wstring(GAMES[i].exe) + L".exe"); }
bool game_ready(int i) { std::error_code ec; return fs::exists(game_exe(i), ec); }

void post_status(const std::wstring& s) { if (g_wnd) PostMessageW(g_wnd, WM_STATUS, 0, (LPARAM) new std::wstring(s)); }

void refresh_tiles() {
    for (int i = 0; i < 3; i++) {
        const Game& g = GAMES[i];
        const wchar_t* state;
        const wchar_t* btn;
        bool enable = !g_busy;
        if (!g.supported) { state = T(L"Bald verfügbar", L"Coming soon"); btn = L"–"; enable = false; }
        else if (game_ready(i)) { state = T(L"Bereit", L"Ready"); btn = T(L"▶  Spielen", L"▶  Play"); }
        else { state = T(L"Noch nicht installiert", L"Not installed yet"); btn = T(L"ISO hinzufügen", L"Add ISO"); }
        SetWindowTextW(g_tile_state[i], state);
        SetWindowTextW(g_tile_btn[i], btn);
        EnableWindow(g_tile_btn[i], enable);
    }
    for (HWND b : {g_btn_settings, g_btn_report}) EnableWindow(b, !g_busy);
    if (g_wnd) InvalidateRect(g_wnd, nullptr, TRUE);
}

extern bool g_settings_page;

// ---------------------------------------------------------------- Einstellungen (settings.ini, liest die Spiel-.exe)
const char* RENDERER_KEYS[] = {"soft", "d3d11", "vulkan", "warp"};
const int SCALES[] = {1, 2, 3, 4, 6, 8};
const int FPS[] = {30, 60, 120, 240};

int xbrz_level_for(int scale) { return scale >= 6 ? 4 : scale >= 4 ? 3 : 2; }   // xBRZ-Stufe nach Skalierung
void xbrz_auto() {   // folgt der Auflösung, bis von Hand geändert
    const int sc = SCALES[std::max((int)SendMessageW(g_cb_scale, CB_GETCURSEL, 0, 0), 0)];
    SendMessageW(g_ck_xbrz, BM_SETCHECK, sc >= 4, 0);
    SendMessageW(g_cb_xbrz_lvl, CB_SETCURSEL, xbrz_level_for(sc) - 2, 0);
    EnableWindow(g_cb_xbrz_lvl, sc >= 4);
}

fs::path settings_path() { return L.cfg / L"settings.ini"; }

// ein Schlüssel in settings.ini, Rest bleibt
void set_setting(const char* key, const std::string& value) {
    std::ifstream in(settings_path());
    std::string all, line;
    bool done = false;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        std::string k = eq == std::string::npos ? std::string() : line.substr(0, eq);
        k.erase(k.find_last_not_of(" \t\r") + 1);
        if (k == key) { line = std::string(key) + " = " + value; done = true; }
        all += line + "\n";
    }
    in.close();
    if (!done) all += std::string(key) + " = " + value + "\n";
    std::ofstream(settings_path()) << all;
}

int default_scale() {
    const int h = GetSystemMetrics(SM_CYSCREEN);
    return h >= 2160 ? 8 : h >= 1440 ? 6 : h >= 1080 ? 4 : 2;
}

void write_default_settings() {
    std::error_code ec;
    if (fs::exists(settings_path(), ec)) return;
    std::ofstream f(settings_path());
    f << "# DIVA PSP Recomp: Einstellungen (schreibt der Launcher, Seite \"Einstellungen\")\n"
      << "window = 1\nrenderer = soft\nscale = " << default_scale() << "\nfps = 30\nxbrz = auto\nlang = " << (g_en ? "en" : "de") << "\n"
      << "save_dir = " << u8(L.saves.wstring()) << "\n";
}

void load_settings_ui() {
    write_default_settings();
    auto m = read_kv(settings_path());
    auto sel = [](HWND cb, int idx) { SendMessageW(cb, CB_SETCURSEL, idx, 0); };
    int r = 1;   // ohne Eintrag: Direct3D 11 (Standard)
    for (int i = 0; i < 4; i++) if (u8(m[L"renderer"]) == RENDERER_KEYS[i]) r = i;
    sel(g_cb_renderer, r);
    const int sc = _wtoi(m[L"scale"].c_str());
    int si = 1;
    for (int i = 0; i < 6; i++) if (SCALES[i] == sc) si = i;
    sel(g_cb_scale, si);
    const int fps = _wtoi(m[L"fps"].c_str());
    int fi = 0;
    for (int i = 0; i < 4; i++) if (FPS[i] == fps) fi = i;
    sel(g_cb_fps, fi);
    // xBRZ: "auto" = an ab 4x (1080p); sonst wie gespeichert
    const std::wstring x = m[L"xbrz"];
    g_xbrz_touched = x == L"on" || x == L"off";
    if (!g_xbrz_touched) xbrz_auto();
    else {
        const int lvl = m.count(L"xbrz_scale") ? std::clamp(_wtoi(m[L"xbrz_scale"].c_str()), 2, 4) : xbrz_level_for(SCALES[si]);
        SendMessageW(g_ck_xbrz, BM_SETCHECK, x == L"on", 0);
        SendMessageW(g_cb_xbrz_lvl, CB_SETCURSEL, lvl - 2, 0);
        EnableWindow(g_cb_xbrz_lvl, x == L"on");
    }
    // Feste Latenz Taste->Bild, 50..500 ms: Runtime verzögert Tasten auf diesen Mittelwert (nie künstlich länger als nötig)
    // leer = Standard (Feld zeigt grau 50)
    const std::wstring lv = m.count(L"latency_ms") ? std::to_wstring(std::clamp(_wtoi(m[L"latency_ms"].c_str()), 50, 500)) : L"";
    SetWindowTextW(g_ed_latency, lv.c_str());
    SendMessageW(g_ck_latency, BM_SETCHECK, m[L"latency"] == L"on", 0);
    EnableWindow(g_ed_latency, m[L"latency"] == L"on");
    EnableWindow(g_btn_lat_reset, m[L"latency"] == L"on");   // nur mit Häkchen
}

std::string latency_ui() {   // leer = Standard, sonst 50..500 ms
    wchar_t buf[16] = {};
    GetWindowTextW(g_ed_latency, buf, 16);
    if (!buf[0]) return "";
    return "latency_ms = " + std::to_string(std::clamp(_wtoi(buf), 50, 500)) + "\n";
}

void save_settings_ui() {
    auto cur = [](HWND cb) { return (int)SendMessageW(cb, CB_GETCURSEL, 0, 0); };
    auto ck = [](HWND c) { return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0; };
    std::ofstream f(settings_path());
    f << "# DIVA PSP Recomp: Einstellungen (schreibt der Launcher, Seite \"Einstellungen\")\n"
      << "window = 1\n"
      << "renderer = " << RENDERER_KEYS[std::max(cur(g_cb_renderer), 0)] << "\n"
      << "scale = " << SCALES[std::max(cur(g_cb_scale), 0)] << "\n"
      << "fps = " << FPS[std::max(cur(g_cb_fps), 0)] << "\n"
      << "xbrz = " << (g_xbrz_touched ? (ck(g_ck_xbrz) ? "on" : "off") : "auto") << "\n"
      << "xbrz_scale = " << (std::max((int)SendMessageW(g_cb_xbrz_lvl, CB_GETCURSEL, 0, 0), 0) + 2) << "\n"
      << "latency = " << (ck(g_ck_latency) ? "on" : "off") << "\n"
      << latency_ui()
      << "lang = " << (g_en ? "en" : "de") << "\n"
      << "save_dir = " << u8(L.saves.wstring()) << "\n";
}

void show_page(bool settings) {
    g_settings_page = settings;
    InvalidateRect(g_wnd, nullptr, TRUE);
    for (HWND h : g_main_ctl) ShowWindow(h, settings ? SW_HIDE : SW_SHOW);
    for (HWND h : g_set_ctl) ShowWindow(h, settings ? SW_SHOW : SW_HIDE);
    for (HWND h : g_test_ctl) ShowWindow(h, SW_HIDE);
    if (g_tip) SendMessageW(g_tip, TTM_ACTIVATE, settings, 0);   // „?“ liegt über einer Kachel
    if (settings) {
        load_settings_ui();
        fill_uninstall_list();
        const std::wstring t = g_state[L"theme"];
        SendMessageW(g_cb_theme, CB_SETCURSEL, t == L"light" ? 1 : t == L"dark" ? 2 : 0, 0);
        SendMessageW(g_cb_lang, CB_SETCURSEL, g_en ? 1 : 0, 0);
        SendMessageW(g_ck_unknown, BM_SETCHECK, g_state[L"unknown_version"] == L"1", 0);
    }
}

void alpha_expired_box();
// Testszenario (launcher.ini): leer = echt, sonst expired / tamper
std::wstring alpha_test_mode();
const char* alpha_test_arg();

// ---------------------------------------------------------------- Bauen
// Prozess starten, Ausgabe ins Bau-Log. PATH nur llvm-mingw, CMake, System (kein MSYS-sh)
int run_logged(const std::wstring& cmdline, const fs::path& cwd, HANDLE log) {
    std::wstring path = (L.tc / L"bin").wstring() + L";" + L.cmake.parent_path().wstring() + L";" +
                        known_folder(FOLDERID_System).wstring() + L";" + known_folder(FOLDERID_Windows).wstring();
    std::wstring env;
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
        if (_wcsnicmp(p, L"PATH=", 5) != 0) { env += p; env.push_back(0); }
    FreeEnvironmentStringsW(block);
    env += L"PATH=" + path; env.push_back(0); env.push_back(0);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::wstring cl = cmdline;
    WriteFile(log, "\r\n> ", 4, nullptr, nullptr);
    const std::string c8 = u8(cl);
    WriteFile(log, c8.data(), (DWORD)c8.size(), nullptr, nullptr);
    WriteFile(log, "\r\n", 2, nullptr, nullptr);
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(),
                        cwd.c_str(), &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

std::wstring q(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

bool is_elf(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    char m[4] = {};
    f.read(m, 4);
    return f && memcmp(m, "\x7f" "ELF", 4) == 0;
}

// EBOOT.BIN, mit PPSSPP entschlüsselt gespeichert (nur bei verschlüsselten UMD-Kopien); das Projekt entschlüsselt nichts
fs::path dumped_eboot(int gi) { return game_dir(gi) / L"EBOOT.BIN"; }

// Fassungen: games\<name>*.toml, Hauptdatei zuerst
std::vector<fs::path> game_tomls(int gi) {
    std::vector<fs::path> v{L.src / L"games" / GAMES[gi].toml};
    const std::wstring stem = fs::path(GAMES[gi].toml).stem().wstring();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(L.src / L"games", ec)) {
        const std::wstring n = e.path().filename().wstring();
        if (e.path().extension() == L".toml" && n.rfind(stem, 0) == 0 && e.path() != v[0]) v.push_back(e.path());
    }
    return v;
}

fs::path own_iso(int gi) { return game_dir(gi) / (std::wstring(GAMES[gi].exe) + L".iso"); }
std::wstring game_iso(int gi) {   // eigene Kopie, sonst gemerkte ISO
    std::error_code ec;
    if (fs::exists(own_iso(gi), ec)) return own_iso(gi).wstring();
    return g_state[std::wstring(L"iso_") + GAMES[gi].key];
}

// Bau-Thread: leer = ok, sonst Fehlertext
std::wstring build_game(int gi, const fs::path& src_iso) {
    std::error_code ec0;
    const fs::path iso = own_iso(gi);
    if (!fs::equivalent(src_iso, iso, ec0)) {   // ISO in den Installationsordner kopieren
        post_status(T(L"ISO kopieren ...", L"Copying ISO ..."));
        fs::create_directories(iso.parent_path(), ec0);
        if (!fs::copy_file(src_iso, iso, fs::copy_options::overwrite_existing, ec0))
            return T(L"Die ISO konnte nicht kopiert werden (genug Speicherplatz?).", L"The ISO could not be copied (enough disk space?).");
    }
    const Game& g = GAMES[gi];
    std::error_code ec;
    if (L.src.empty()) return T(L"Quellcode nicht gefunden (Ordner src\\ fehlt).", L"Source code not found (folder src\\ is missing).");
    if (L.tc.empty()) return T(L"Compiler (llvm-mingw) nicht gefunden.", L"Compiler (llvm-mingw) not found.");
    if (L.cmake.empty()) return T(L"CMake nicht gefunden.", L"CMake not found.");
    if (L.recomp.empty()) return T(L"Recompiler (diva_recomp.exe) nicht gefunden.", L"Recompiler (diva_recomp.exe) not found.");
    const fs::path work = L.data / L"work" / g.key;
    fs::create_directories(work, ec);
    const fs::path logp = L.data / L"work" / (std::wstring(g.key) + L"-build.log");
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(logp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    struct Closer { HANDLE h; ~Closer() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } closer{log};
    const std::wstring see_log = L"\nDetails: " + logp.wstring();

    post_status(T(L"1/4  Programm aus der ISO lesen ...", L"1/4  Reading the program from the ISO ..."));
    fs::path eboot = work / L"EBOOT.BIN";
    if (!iso_extract(iso, "PSP_GAME/SYSDIR/EBOOT.BIN", eboot)) return T(L"EBOOT.BIN konnte nicht aus der ISO gelesen werden.", L"EBOOT.BIN could not be read from the ISO.");
    if (is_elf(eboot)) {   // unverschlüsselt in der ISO: eine alte EBOOT aus PPSSPP würde beim Spielen falsch mitgegeben
        std::error_code e2;
        fs::remove(dumped_eboot(gi), e2);
    } else {   // verschlüsselt -> EBOOT aus PPSSPP
        eboot = dumped_eboot(gi);
        if (!is_elf(eboot))
            return T(L"Das Programm in der ISO ist verschlüsselt und die entschlüsselte EBOOT.BIN (aus PPSSPP) fehlt.\n"
                     L"Bitte die ISO erneut hinzufügen, der Launcher fragt dann nach der Datei.",
                     L"The program in this ISO is encrypted and the decrypted EBOOT.BIN (from PPSSPP) is missing.\n"
                     L"Please add the ISO again, the launcher will then ask for the file.");
    }
    // Fassung erkennen: games\<spiel>*.toml gegen die Prüfsumme (diva_recomp --check-only)
    fs::path toml;
    for (const fs::path& c : game_tomls(gi))
        if (run_logged(q(L.recomp) + L" --game " + q(c) + L" --eboot " + q(eboot) + L" --check-only", work, log) == 0) { toml = c; break; }
    const bool unknown = toml.empty() && g_unknown_ok;   // auf eigenes Risiko: mit der Standard-Konfiguration
    if (unknown) toml = game_tomls(gi)[0];
    if (toml.empty())
        return T(L"Diese Fassung des Spiels wird noch nicht unterstützt (unbekanntes Programm, z.B. andere Version oder anderer Patch).",
                 L"This version of the game is not supported yet (unknown program, e.g. a different version or patch).") + see_log;
    post_status(T(L"2/4  Spielcode übersetzen (Recompiler) ...", L"2/4  Translating the game code (recompiler) ..."));
    const fs::path gen = work / L"generated" / g.exe;
    if (run_logged(q(L.recomp) + L" --game " + q(toml) + L" --eboot " + q(eboot) + L" --out " + q(gen) + (unknown ? L" --ignore-hash" : L""), work, log) != 0)
        return T(L"Der Recompiler ist fehlgeschlagen.", L"The recompiler failed.") + see_log;
    post_status(T(L"3/4  Bauen vorbereiten (CMake) ...", L"3/4  Preparing the build (CMake) ..."));
    const fs::path bdir = work / L"build";
    // immer frisch bauen (sonst "duplicate symbol")
    fs::remove_all(bdir, ec);
    if (run_logged(q(L.cmake) + L" -S " + q(L.src) + L" -B " + q(bdir) +
                       L" -G \"MinGW Makefiles\" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release"
                       L" -DBUILD_TESTING=OFF -DCMAKE_MAKE_PROGRAM=" + q(L.tc / L"bin" / L"mingw32-make.exe") + L" -DDIVA_GAME_DIR=" + q(gen),
                   work, log) != 0)
        return T(L"CMake ist fehlgeschlagen.", L"CMake failed.") + see_log;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    post_status(T(L"4/4  Spiel kompilieren (dauert etwas) ...", L"4/4  Compiling the game (takes a while) ..."));
    if (run_logged(q(L.cmake) + L" --build " + q(bdir) + L" --target " + g.exe + L" -j " + std::to_wstring(si.dwNumberOfProcessors), work, log) != 0)
        return T(L"Das Kompilieren ist fehlgeschlagen.", L"Compiling failed.") + see_log;
    // Ergebnis kopieren: Exe + DLLs
    post_status(T(L"Fertigstellen ...", L"Finishing ..."));
    const fs::path out = game_dir(gi);
    fs::create_directories(out, ec);
    const fs::path built = bdir / L"runtime" / (std::wstring(g.exe) + L".exe");
    if (!fs::exists(built, ec)) return T(L"Die gebaute Spiel-Exe fehlt.", L"The built game exe is missing.") + see_log;
    fs::copy_file(built, game_exe(gi), fs::copy_options::overwrite_existing, ec);
    if (ec) return T(L"Spiel-Exe konnte nicht kopiert werden (läuft das Spiel noch?).", L"The game exe could not be copied (is the game still running?).");
    for (const fs::path& dll : {L.tc / L"x86_64-w64-mingw32" / L"bin" / L"libc++.dll", L.tc / L"x86_64-w64-mingw32" / L"bin" / L"libunwind.dll",
                                L.src / L"third_party" / L"SDL3" / L"lib" / L"x64" / L"SDL3.dll"})
        if (fs::exists(dll, ec)) fs::copy_file(dll, out / dll.filename(), fs::copy_options::overwrite_existing, ec);
    return {};
}

void start_build(int gi, const fs::path& iso) {
    g_busy = true;
    g_unknown_ok = g_state[L"unknown_version"] == L"1";
    refresh_tiles();
    SendMessageW(g_progress, PBM_SETMARQUEE, TRUE, 30);
    ShowWindow(g_progress, SW_SHOW);
    std::thread([gi, iso] {
        const std::wstring err = build_game(gi, iso);
        PostMessageW(g_wnd, WM_BUILD_DONE, gi, (LPARAM) new std::wstring(err));
    }).detach();
}

// verschlüsselte ISO: EBOOT.BIN aus PPSSPP nötig; false = abgebrochen
bool ensure_eboot(int gi, const fs::path& iso) {
    std::error_code ec;
    const fs::path tmp = L.data / L"work" / (std::wstring(GAMES[gi].key) + L"-check.bin");
    fs::create_directories(tmp.parent_path(), ec);
    const bool ok = iso_extract(iso, "PSP_GAME/SYSDIR/EBOOT.BIN", tmp);
    const bool plain = ok && is_elf(tmp);
    fs::remove(tmp, ec);
    if (!ok || plain) return true;   // nicht lesbar: meldet der Bau
    const std::wstring id = fs::path(std::u8string((const char8_t*)GAMES[gi].disc)).wstring();
    if (msg(T(L"Das Programm in dieser ISO ist verschlüsselt (normal bei einer Kopie der Original-UMD).\n\n"
              L"So geht es weiter (einmalig, ca. 5 Minuten):\n"
              L"1. PPSSPP öffnen: Einstellungen → Werkzeuge → Entwicklerwerkzeuge → das Speichern (Dump) der EBOOT einschalten.\n"
              L"2. Das Spiel in PPSSPP bis zum Titelbild starten. PPSSPP meldet \"Dump Decrypted Eboot\" mit dem Speicherort.\n"
              L"3. Hier die Datei ",
              L"The program in this ISO is encrypted (normal for a copy of the original UMD).\n\n"
              L"How to continue (once, about 5 minutes):\n"
              L"1. Open PPSSPP: Settings → Tools → Developer tools → turn on dumping the EBOOT.\n"
              L"2. Start the game in PPSSPP until the title screen. PPSSPP shows \"Dump Decrypted Eboot\" with the location.\n"
              L"3. Select the file ") + id + T(L"_EBOOT.BIN auswählen (Ordner PSP\\SYSTEM\\DUMP des PPSSPP-Memory-Sticks).\n\n"
              L"Genaue Anleitung: Tester-Anleitung, Abschnitt \"Verschlüsselte ISO\". Datei jetzt auswählen?",
              L"_EBOOT.BIN here (folder PSP\\SYSTEM\\DUMP of the PPSSPP memory stick).\n\n"
              L"Detailed steps: tester guide, section \"Encrypted ISO\". Select the file now?"),
            T(L"ISO hinzufügen", L"Add ISO"), IDI_WARNING_, MB_OKCANCEL) != IDOK)
        return false;
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_wnd;
    of.lpstrFilter = g_en ? L"Decrypted EBOOT (*.BIN)\0*.BIN\0All files\0*.*\0" : L"Entschlüsselte EBOOT (*.BIN)\0*.BIN\0Alle Dateien\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = T(L"Entschlüsselte EBOOT.BIN aus PPSSPP auswählen", L"Select the decrypted EBOOT.BIN from PPSSPP");
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return false;
    if (!is_elf(file)) {
        msg(T(L"Diese Datei ist kein entschlüsseltes Programm. Bitte die Datei aus PPSSPP (…_EBOOT.BIN) wählen.",
              L"This file is not a decrypted program. Please select the file from PPSSPP (…_EBOOT.BIN)."),
            T(L"ISO hinzufügen", L"Add ISO"), IDI_ERROR_);
        return false;
    }
    fs::create_directories(game_dir(gi), ec);
    fs::copy_file(file, dumped_eboot(gi), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        msg(T(L"Die Datei konnte nicht kopiert werden.", L"The file could not be copied."), T(L"ISO hinzufügen", L"Add ISO"), IDI_ERROR_);
        return false;
    }
    return true;
}

// ISO prüfen und dem passenden Spiel zuordnen
void add_iso(const fs::path& iso) {
    if (g_busy) return;
    const std::string id = iso_disc_id(iso);
    if (id.empty()) {
        msg(T(L"Das ist keine lesbare PSP-ISO.", L"This is not a readable PSP ISO."), T(L"ISO hinzufügen", L"Add ISO"), IDI_ERROR_);
        return;
    }
    for (int i = 0; i < 3; i++)
        if (id == GAMES[i].disc) {
            if (!GAMES[i].supported) {
                msg(std::wstring(GAMES[i].title) + T(L" wurde erkannt, wird aber noch nicht unterstützt.", L" was recognized but is not supported yet."), T(L"ISO hinzufügen", L"Add ISO"), IDI_WARNING_);
                return;
            }
            if (!ensure_eboot(i, iso)) return;   // UMD-Kopie: EBOOT aus PPSSPP
            g_state[std::wstring(L"iso_") + GAMES[i].key] = own_iso(i).wstring();   // eigene Kopie
            save_state();
            start_build(i, iso);
            return;
        }
    msg(T(L"Unbekannte Spiel-ID: ", L"Unknown game ID: ") + fs::path(std::u8string(id.begin(), id.end())).wstring() +
            T(L"\nUnterstützt: Project DIVA 1st (ULJM05472).", L"\nSupported: Project DIVA 1st (ULJM05472)."), T(L"ISO hinzufügen", L"Add ISO"), IDI_ERROR_);
}

void pick_iso() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_wnd;
    of.lpstrFilter = g_en ? L"PSP ISO (*.iso)\0*.iso\0All files\0*.*\0" : L"PSP-ISO (*.iso)\0*.iso\0Alle Dateien\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = T(L"ISO auswählen (deine eigene Kopie des Spiels)", L"Select ISO (your own copy of the game)");
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&of)) add_iso(file);
}

void play(int gi) {
    const std::wstring iso = game_iso(gi);
    std::error_code ec;
    if (iso.empty() || !fs::exists(iso, ec)) {
        msg(T(L"Die ISO wurde verschoben oder gelöscht. Bitte wähle sie neu aus.", L"The ISO was moved or deleted. Please select it again."), T(L"Spielen", L"Play"), IDI_ERROR_);
        pick_iso();
        return;
    }
    if (alpha::check(alpha_test_arg()).state == alpha::State::Expired) {   // T90 Fall A
        alpha_expired_box();
        return;
    }
    write_default_settings();
    std::wstring cl = q(game_exe(gi)) + L" --iso " + q(iso) + L" --settings " + q(settings_path()) + L" --save-dir " + q(L.saves) + L" --log " +
                      q(L.data / L"work" / (std::wstring(GAMES[gi].key) + L"-run.log"));
    if (!alpha_test_mode().empty()) cl += L" --alpha-test " + alpha_test_mode();   // Testszenario
    if (is_elf(dumped_eboot(gi))) cl += L" --eboot " + q(dumped_eboot(gi));   // Programm aus PPSSPP
    // ohne Konsole; Ausgabe in work\<spiel>-start.txt, bei Fehler in <5 s zeigt sie der Launcher
    const fs::path outp = L.data / L"work" / (std::wstring(GAMES[gi].key) + L"-start.txt");
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(outp.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        const std::string c8 = "> " + u8(cl) + "\r\n";
        WriteFile(out, c8.data(), (DWORD)c8.size(), nullptr, nullptr);
    }
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi = {};
    const std::wstring cwd = game_dir(gi).wstring();
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) {
        msg(T(L"Das Spiel konnte nicht gestartet werden.", L"The game could not be started."), T(L"Spielen", L"Play"), IDI_ERROR_);
        return;
    }
    CloseHandle(pi.hThread);
    std::thread([proc = pi.hProcess, outp] {
        DWORD code = 0;
        const bool early = WaitForSingleObject(proc, 5000) == WAIT_OBJECT_0;
        if (!early) WaitForSingleObject(proc, INFINITE);   // Kreuz/Alt+F4 = 0
        if (!early && GetExitCodeProcess(proc, &code) && code != 0) PostMessageW(g_wnd, WM_GAME_CRASHED, code, 0);
        else if (early && GetExitCodeProcess(proc, &code) && code != 0 && code != 60) {
            std::ifstream f(outp);
            std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (all.size() > 1500) all = all.substr(0, 1500);
            PostMessageW(g_wnd, WM_GAME_FAILED, code, (LPARAM) new std::wstring(fs::path(std::u8string(all.begin(), all.end())).wstring()));
        }
        CloseHandle(proc);
    }).detach();
}

// ---------------------------------------------------------------- Hilfen: Programme ohne Fenster, Systeminfos
// ohne Konsolenfenster ausführen; cwd, damit tar.exe nur kurze ASCII-Pfade sieht (Umlaute im Benutzernamen)
int run_hidden(std::wstring cl, const fs::path& cwd, DWORD timeout_ms = INFINITE) {
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
        return -1;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
fs::path tar_exe() { return known_folder(FOLDERID_System) / L"tar.exe"; }   // ab Win10 1803

std::wstring reg_text(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t b[256] = {};
    DWORD size = sizeof b;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, b, &size) != ERROR_SUCCESS) return L"?";
    std::wstring v = b;
    v.erase(v.find_last_not_of(L' ') + 1);   // trailing Leerzeichen
    return v;
}

// PC-Kurzinfo für Fehlerberichte
std::string system_info() {
    std::wstring t = L"DIVA PSP Recomp " + std::wstring(VERSION) + L"\r\n";
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW ov = {sizeof ov};
    if (auto f = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&ov);
    t += L"Windows: " + std::to_wstring(ov.dwMajorVersion) + L"." + std::to_wstring(ov.dwMinorVersion) + L" Build " +
         std::to_wstring(ov.dwBuildNumber) + L" (" + reg_text(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion") + L")\r\n";
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    t += L"CPU: " + reg_text(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString") + L", " +
         std::to_wstring(si.dwNumberOfProcessors) + L" Threads\r\n";
    MEMORYSTATUSEX ms = {sizeof ms};
    GlobalMemoryStatusEx(&ms);
    t += L"RAM: " + std::to_wstring(ms.ullTotalPhys >> 20) + L" MB\r\n";
    DISPLAY_DEVICEW dd = {sizeof dd};
    std::wstring gpus;   // eine Zeile je Karte
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof dd)
        if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && gpus.find(dd.DeviceString) == std::wstring::npos)
            gpus += L"GPU: " + std::wstring(dd.DeviceString) + L"\r\n";
    t += gpus;
    t += L"Bildschirm: " + std::to_wstring(GetSystemMetrics(SM_CXSCREEN)) + L"x" + std::to_wstring(GetSystemMetrics(SM_CYSCREEN)) +
         L", DPI " + std::to_wstring(GetDpiForWindow(g_wnd ? g_wnd : GetDesktopWindow())) + L"\r\n";
    t += L"Sprache: " + std::wstring(g_en ? L"en" : L"de") + L", Windows-Sprache " + std::to_wstring(GetUserDefaultUILanguage()) + L"\r\n";
    for (int i = 0; i < 3; i++)
        if (game_ready(i)) t += std::wstring(GAMES[i].title) + L": gebaut mit " + g_state[std::wstring(L"built_") + GAMES[i].key] + L"\r\n";
    const alpha::Result ar = alpha::check(nullptr, 1500);
    t += L"Alpha: Zustand " + std::to_wstring((int)ar.state) + L", Ablauf " + std::to_wstring(ar.expires) + L", Zeitserver " +
         std::to_wstring(ar.real) + L", PC-Uhr " + std::to_wstring(ar.local) + L"\r\n";
    return u8(t);
}

// kopieren; über max nur das Ende (Log)
void copy_tail(const fs::path& from, const fs::path& to, uintmax_t max) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(from, ec);
    if (ec) return;
    if (size <= max) { fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec); return; }
    std::ifstream in(from, std::ios::binary);
    in.seekg((std::streamoff)(size - max));
    std::vector<char> b((size_t)max);
    in.read(b.data(), (std::streamsize)b.size());
    std::ofstream out(to, std::ios::binary);
    const std::string head = "[... gekürzt, nur die letzten " + std::to_string(max >> 20) + " MB ...]\n";
    out.write(head.data(), (std::streamsize)head.size());
    out.write(b.data(), in.gcount());
}

// ---------------------------------------------------------------- Fehler melden
// Logs, Einstellungen, PC-Infos als ZIP auf den Desktop
void report_bug() {
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));   // Sanduhr
    std::error_code ec;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring name = std::wstring(T(L"DIVA-Fehlerbericht-", L"DIVA-bug-report-")) + stamp;
    const fs::path tmp = L.data / L"work" / L"report";   // temporär
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / L"files", ec);
    for (auto& e : fs::directory_iterator(L.data / L"work", ec)) {
        const std::wstring ext = e.path().extension().wstring();
        if (e.is_regular_file(ec) && (ext == L".log" || ext == L".txt")) copy_tail(e.path(), tmp / L"files" / e.path().filename(), 20u << 20);
    }
    for (const wchar_t* n : {L"settings.ini", L"launcher.ini"})
        if (fs::exists(L.cfg / n, ec)) fs::copy_file(L.cfg / n, tmp / L"files" / n, fs::copy_options::overwrite_existing, ec);
    std::ofstream(tmp / L"files" / L"system.txt", std::ios::binary) << system_info();
    // packen, dann auf den Desktop
    std::wstring names;
    for (auto& e : fs::directory_iterator(tmp / L"files", ec)) names += L" " + q(e.path().filename().wstring());
    const int rc = run_hidden(q(tar_exe()) + L" -a -c -f report.zip -C files" + names, tmp, 120000);
    fs::path out = known_folder(FOLDERID_Desktop) / (name + L".zip");
    if (rc == 0 && fs::exists(tmp / L"report.zip", ec)) {
        fs::copy_file(tmp / L"report.zip", out, fs::copy_options::overwrite_existing, ec);
        if (ec) out.clear();
    } else out.clear();
    if (out.empty()) {   // ohne tar.exe: Ordner statt ZIP
        out = known_folder(FOLDERID_Desktop) / name;
        fs::copy(tmp / L"files", out, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    fs::remove_all(tmp, ec);
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
    const std::wstring to = *REPORT_TO ? std::wstring(REPORT_TO) : std::wstring(T(L"(wird beim Alpha-Start bekannt gegeben)", L"(will be announced when the alpha starts)"));
    msg(T(L"Fehlerbericht gespeichert:\n", L"Bug report saved:\n") + out.wstring() +
            T(L"\n\nBitte schick diese Datei an: ", L"\n\nPlease send this file to: ") + to +
            T(L"\n\nSchreib kurz dazu: Was hast du gemacht, was ist passiert, und was hättest du erwartet? "
              L"Der Bericht enthält Logs, Einstellungen und Infos zu deinem PC (Windows, CPU, Grafikkarte), keine Spieldateien.",
              L"\n\nPlease add a short note: what did you do, what happened, and what did you expect? "
              L"The report contains logs, settings and info about your PC (Windows, CPU, graphics card), no game files."),
        T(L"Fehler melden", L"Report a bug"), IDI_SUCCESS);
    const std::wstring sel = L"/select,\"" + out.wstring() + L"\"";
    ShellExecuteW(g_wnd, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
}

void about() {
    std::wstring t = L"DIVA PSP Recomp " + std::wstring(VERSION) +
                     T(L"\n\nDie PSP-Spiele Hatsune Miku: Project DIVA, statisch recompiliert und nativ unter Windows.\n"
                       L"Kein Emulator. Keine Spieldateien enthalten: Du brauchst deine eigene Kopie.\n\n"
                       L"Lead-Developer: (Name folgt)\nTester: (Namen folgen)\n\n"
                       L"Lizenz: CC BY-NC 4.0 (eigener Code), Teile von Dritten mit eigener Lizenz (siehe LICENSE).\n"
                       L"Icons: Paulina T, Set \"Iconic Vocal Synth Songs\", Noun Project, CC BY 3.0 (angepasst).\n"
                       L"Hatsune Miku © Crypton Future Media, INC. www.piapro.net\n"
                       L"Inoffizielles Fan-Projekt: kein Produkt von SEGA oder Crypton Future Media, nicht von ihnen unterstützt. "
                       L"Keine Spieldateien enthalten; du brauchst deine eigene Kopie.\n\n"
                       L"Daten: ",
                       L"\n\nThe PSP games Hatsune Miku: Project DIVA, statically recompiled and running natively on Windows.\n"
                       L"Not an emulator. No game files included: you need your own copy.\n\n"
                       L"Lead developer: (name to follow)\nTesters: (names to follow)\n\n"
                       L"License: CC BY-NC 4.0 (own code), third-party parts under their own licenses (see LICENSE).\n"
                       L"Icons: Paulina T, set \"Iconic Vocal Synth Songs\", Noun Project, CC BY 3.0 (modified).\n"
                       L"Hatsune Miku © Crypton Future Media, INC. www.piapro.net\n"
                       L"Unofficial fan project: not a product of SEGA or Crypton Future Media, not endorsed by them. "
                       L"No game files included; you need your own copy.\n\n"
                       L"Data: ") +
                     L.data.wstring() + T(L"\nSpielstände: ", L"\nSave data: ") + L.saves.wstring();
    // Danksagungen, aufklappbar
    const wchar_t* thanks = T(
        L"Danke an alle, auf deren Arbeit dieses Projekt aufbaut:\n\n"
        L"• PPSSPP (Henrik Rydgård und Mitwirkende, GPL 2.0 oder neuer): Vorbild und Nachschlagewerk für das Verhalten der "
        L"PSP-Systemfunktionen, Rückgabewerte und Grafikformate; die Liste der Funktionskennungen (NIDs); der ATRAC3-Decoder "
        L"in der Form aus PPSSPP. PPSSPP ist auch das Werkzeug, mit dem du die entschlüsselte EBOOT.BIN aus deinem Spiel speicherst.\n"
        L"• FFmpeg (LGPL 2.1 oder neuer): ATRAC3/ATRAC3plus-Decoder für den Ton der Videos (third_party/at3_standalone).\n"
        L"• SDL3 (Sam Lantinga und Mitwirkende, zlib-Lizenz): Controller (PlayStation, Xbox, Switch und viele mehr).\n"
        L"• llvm-mingw (Martin Storsjö) mit LLVM/Clang (Apache 2.0 mit LLVM-Ausnahme) und mingw-w64: der Compiler, der das Spiel "
        L"auf deinem PC baut.\n"
        L"• CMake (Kitware, BSD-3-Clause): steuert den Bau.\n"
        L"• Ghidra (NSA, Apache 2.0) mit dem Allegrex-Plugin ghidra-allegrex (kotcrab, Apache 2.0): Analyse des Spielcodes "
        L"während der Entwicklung.\n"
        L"• pspdev / PSPSDK mit GCC und newlib (Open Source): Bau des eigenen Latenz-Tests für die PSP.\n"
        L"• Python (Python Software Foundation): Hilfsskripte für Analyse und Setup.\n"
        L"• Microsoft Windows: Direct3D 11 (Grafik), WASAPI (Ton), Media Foundation (Videos), WinHTTP (Updates), "
        L"Schriften Segoe UI und Segoe MDL2 Assets.\n"
        L"• Icons: Paulina T, Set \"Iconic Vocal Synth Songs\", Noun Project, CC BY 3.0 (angepasst).\n"
        L"• Die PSP-Homebrew- und Emulations-Community, deren jahrelange Arbeit das Wissen über die PSP überhaupt erst "
        L"möglich gemacht hat.\n\n"
        L"Hatsune Miku: Project DIVA © SEGA. Hatsune Miku © Crypton Future Media, INC. www.piapro.net\n"
        L"Lizenztexte: Datei LICENSE und die Ordner der jeweiligen Projekte im Installationsordner (src\\third_party, tools).",
        L"Thanks to everyone whose work this project builds on:\n\n"
        L"• PPSSPP (Henrik Rydgård and contributors, GPL 2.0 or later): model and reference for the behavior of the PSP system "
        L"functions, return values and graphics formats; the list of function IDs (NIDs); the ATRAC3 decoder in the form used by "
        L"PPSSPP. PPSSPP is also the tool you use to save the decrypted EBOOT.BIN from your game.\n"
        L"• FFmpeg (LGPL 2.1 or later): ATRAC3/ATRAC3plus decoder for the sound of the videos (third_party/at3_standalone).\n"
        L"• SDL3 (Sam Lantinga and contributors, zlib license): controllers (PlayStation, Xbox, Switch and many more).\n"
        L"• llvm-mingw (Martin Storsjö) with LLVM/Clang (Apache 2.0 with LLVM exception) and mingw-w64: the compiler that builds "
        L"the game on your PC.\n"
        L"• CMake (Kitware, BSD-3-Clause): drives the build.\n"
        L"• Ghidra (NSA, Apache 2.0) with the Allegrex plugin ghidra-allegrex (kotcrab, Apache 2.0): analysis of the game code "
        L"during development.\n"
        L"• pspdev / PSPSDK with GCC and newlib (open source): building our own latency test for the PSP.\n"
        L"• Python (Python Software Foundation): helper scripts for analysis and setup.\n"
        L"• Microsoft Windows: Direct3D 11 (graphics), WASAPI (sound), Media Foundation (videos), WinHTTP (updates), "
        L"fonts Segoe UI and Segoe MDL2 Assets.\n"
        L"• Icons: Paulina T, set \"Iconic Vocal Synth Songs\", Noun Project, CC BY 3.0 (modified).\n"
        L"• The PSP homebrew and emulation community, whose years of work made knowing the PSP possible in the first place.\n\n"
        L"Hatsune Miku: Project DIVA © SEGA. Hatsune Miku © Crypton Future Media, INC. www.piapro.net\n"
        L"License texts: file LICENSE and the folders of each project in the installation folder (src\\third_party, tools).");
    TASKDIALOGCONFIG tc = {sizeof tc};
    tc.hwndParent = g_wnd;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.dwFlags = TDF_USE_HICON_MAIN | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_EXPAND_FOOTER_AREA;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.pszWindowTitle = T(L"Über", L"About");
    tc.hMainIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_ABOUT), IMAGE_ICON, 32, 32, 0);
    tc.pszContent = t.c_str();
    tc.pszExpandedInformation = thanks;
    tc.pszCollapsedControlText = T(L"Open Source und Danksagungen anzeigen", L"Show open source and acknowledgements");
    tc.pszExpandedControlText = T(L"Open Source und Danksagungen ausblenden", L"Hide open source and acknowledgements");
    tc.cxWidth = 300;   // Dialog-Einheiten
    using Fn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "TaskDialogIndirect");
    if (!fn || FAILED(fn(&tc, nullptr, nullptr, nullptr))) msg(t + L"\n\n" + thanks, T(L"Über", L"About"), IDI_ABOUT);
}

// ---------------------------------------------------------------- Deinstallieren
void fill_uninstall_list() {   // Knöpfe nur wenn was da ist
    bool any = false;
    for (int i = 0; i < 3; i++) any |= game_ready(i);
    EnableWindow(g_btn_uninst_game, any);
    std::error_code ec;
    EnableWindow(g_btn_del_saves, fs::exists(L.saves, ec) && !fs::is_empty(L.saves, ec));
}

bool remove_dir(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    return !ec;
}

void uninstall_game() {
    // Menü mit installierten Spielen
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < 3; i++)
        if (game_ready(i)) AppendMenuW(m, MF_STRING, 1 + i, GAMES[i].title);
    RECT rc;
    GetWindowRect(g_btn_uninst_game, &rc);
    const int pick = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, rc.left, rc.bottom, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (pick < 1) return;
    const int gi = pick - 1;
    if (msg(g_en ? L"Uninstall " + std::wstring(GAMES[gi].title) + L"?\n\nThe built game will be deleted. Your ISO and your save data are kept; "
                   L"you can reinstall it at any time."
                 : std::wstring(GAMES[gi].title) + L" deinstallieren?\n\nDas gebaute Spiel wird gelöscht. Deine ISO und deine Spielstände bleiben erhalten; "
                   L"du kannst es jederzeit neu installieren.",
            T(L"Spiel deinstallieren", L"Uninstall game"), IDI_UNINSTALL, MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    const bool ok = remove_dir(game_dir(gi)) & remove_dir(L.data / L"work" / GAMES[gi].key);
    g_state.erase(std::wstring(L"iso_") + GAMES[gi].key);
    g_state.erase(std::wstring(L"built_") + GAMES[gi].key);
    save_state();
    refresh_tiles();
    fill_uninstall_list();
    if (ok) msg(std::wstring(GAMES[gi].title) + T(L" wurde deinstalliert.", L" was uninstalled."), T(L"Spiel deinstallieren", L"Uninstall game"), IDI_SUCCESS);
    else msg(T(L"Nicht alles konnte gelöscht werden. Läuft das Spiel noch?", L"Not everything could be deleted. Is the game still running?"),
             T(L"Spiel deinstallieren", L"Uninstall game"), IDI_ERROR_);
}

void delete_saves() {
    if (msg(T(L"Alle Spielstände löschen?\n\nDas kann nicht rückgängig gemacht werden.\nOrdner: ",
              L"Delete all save data?\n\nThis cannot be undone.\nFolder: ") + L.saves.wstring(),
            T(L"Spielstände löschen", L"Delete save data"), IDI_WARNING_, MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    std::error_code ec;
    bool ok = true;
    for (auto& e : fs::directory_iterator(L.saves, ec)) { fs::remove_all(e.path(), ec); if (ec) ok = false; }
    fill_uninstall_list();
    if (ok) msg(T(L"Alle Spielstände wurden gelöscht.", L"All save data was deleted."), T(L"Spielstände löschen", L"Delete save data"), IDI_SUCCESS);
    else msg(T(L"Nicht alles konnte gelöscht werden. Läuft das Spiel noch?", L"Not everything could be deleted. Is the game still running?"),
             T(L"Spielstände löschen", L"Delete save data"), IDI_ERROR_);
}

// Registry-Key des Setups ("Apps"), Startmenü-Link
const wchar_t* const UNINSTALL_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DIVA PSP Recomp";
fs::path start_menu_link() { return known_folder(FOLDERID_Programs) / L"DIVA PSP Recomp.lnk"; }
fs::path desktop_link() { return known_folder(FOLDERID_Desktop) / L"DIVA PSP Recomp.lnk"; }
bool is_installed() {   // Setup, kein Checkout
    std::error_code ec;
    return fs::equivalent(L.root, L.data, ec);
}

// alles weg: Spiele, Bau, Einstellungen, Links, Apps-Eintrag, optional Spielstände; die laufende Exe löscht ein cmd danach
bool uninstall_all() {
    if (msg(T(L"DIVA PSP Recomp komplett deinstallieren?\n\nGelöscht werden alle installierten Spiele, Einstellungen und die Verknüpfungen. "
              L"Deine ISO-Dateien bleiben unberührt.",
              L"Uninstall DIVA PSP Recomp completely?\n\nAll installed games, settings and the shortcuts will be deleted. "
              L"Your ISO files are not touched."),
            T(L"Launcher deinstallieren", L"Uninstall launcher"), IDI_UNINSTALL, MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return false;
    std::error_code ec;
    const bool has_saves = fs::exists(L.saves, ec) && !fs::is_empty(L.saves, ec);
    bool keep_saves = false;
    if (has_saves) {
        const int a = msg(T(L"Spielstände behalten?\n\nJa: Spielstände bleiben im Ordner\n", L"Keep your save data?\n\nYes: save data stays in the folder\n") +
                              L.saves.wstring() +
                              T(L"\nNein: Spielstände werden ebenfalls gelöscht (nicht rückgängig zu machen).",
                                L"\nNo: save data is deleted as well (cannot be undone)."),
                          T(L"Launcher deinstallieren", L"Uninstall launcher"), IDI_WARNING_, MB_YESNOCANCEL);
        if (a == IDCANCEL) return false;
        keep_saves = a == IDYES;
    }
    // nur der eigene Ordner
    if (L.data.filename() != L"DIVA PSP Recomp" || L.data.parent_path() != known_folder(FOLDERID_LocalAppData)) {
        msg(T(L"Unerwarteter Installationsordner, es wird nichts gelöscht:\n", L"Unexpected install folder, nothing is deleted:\n") +
                L.data.wstring(), T(L"Launcher deinstallieren", L"Uninstall launcher"), IDI_ERROR_);
        return false;
    }
    const bool installed = is_installed();
    fs::remove(desktop_link(), ec);
    fs::remove(start_menu_link(), ec);
    RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    for (auto& e : fs::directory_iterator(L.data, ec))
        if (!(keep_saves && e.path().filename() == L"saves")) fs::remove_all(e.path(), ec);   // laufende Exe bleibt
    if (installed) {   // danach Exe (ggf. Ordner) löschen
        // cmd /s /c "...": nur das äußere Quote-Paar fällt weg
        std::wstring cl = L"\"" + (known_folder(FOLDERID_System) / L"cmd.exe").wstring() + L"\" /d /s /c \"\"" +
                          (known_folder(FOLDERID_System) / L"ping.exe").wstring() + L"\" -n 3 127.0.0.1 >nul & ";
        if (keep_saves) cl += L"del /f /q \"" + (L.data / L"DIVA Launcher.exe").wstring() + L"\"\"";
        else cl += L"rmdir /s /q \"" + L.data.wstring() + L"\"\"";
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, known_folder(FOLDERID_LocalAppData).c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    msg(keep_saves ? T(L"Deinstalliert. Deine Spielstände liegen weiter in\n", L"Uninstalled. Your save data is still in\n") + L.saves.wstring()
                   : std::wstring(T(L"Deinstalliert.", L"Uninstalled.")),
        T(L"Launcher deinstallieren", L"Uninstall launcher"), IDI_SUCCESS);
    if (g_wnd) DestroyWindow(g_wnd);
    return true;
}

// ---------------------------------------------------------------- Hinweis beim Start (inoffizielles Fan-Projekt)
#define DISCLAIMER_TITLE T(L"Inoffizielles Fan-Projekt", L"Unofficial fan project")
#define DISCLAIMER_TEXT T( \
    L"DIVA PSP Recomp ist kein Produkt von SEGA oder Crypton Future Media und steht in keiner Verbindung zu ihnen. " \
    L"Es wird von ihnen weder unterstützt noch geprüft. Es ist ein kostenloses, nicht kommerzielles Fan-Projekt.\n\n" \
    L"Es sind keine Spieldateien enthalten, und es werden keine verteilt. Du brauchst deine eigene, rechtmäßig erworbene " \
    L"Kopie des Spiels. Das Spiel wird nur auf deinem PC aus deiner eigenen Kopie gebaut.\n\n" \
    L"Hatsune Miku: Project DIVA © SEGA. Hatsune Miku © Crypton Future Media, INC.", \
    L"DIVA PSP Recomp is not a product of SEGA or Crypton Future Media and is not affiliated with them. " \
    L"It is neither endorsed nor reviewed by them. It is a free, non-commercial fan project.\n\n" \
    L"No game files are included, and none are distributed. You need your own, legally obtained copy of the game. " \
    L"The game is built only on your PC from your own copy.\n\n" \
    L"Hatsune Miku: Project DIVA © SEGA. Hatsune Miku © Crypton Future Media, INC.")

void show_disclaimer(bool on_start) {
    if (on_start && g_state[L"disclaimer_hide"] == L"1") return;
    TASKDIALOGCONFIG tc = {sizeof tc};
    tc.hwndParent = g_wnd;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.dwFlags = TDF_USE_HICON_MAIN | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.pszWindowTitle = L"DIVA PSP Recomp";
    tc.hMainIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_WARNING_), IMAGE_ICON, 32, 32, 0);
    tc.pszMainInstruction = DISCLAIMER_TITLE;
    tc.pszContent = DISCLAIMER_TEXT;
    if (on_start) tc.pszVerificationText = T(L"Nicht erneut anzeigen", L"Don't show again");
    BOOL hide = FALSE;
    // TaskDialog braucht Common Controls 6, sonst einfache Meldung
    using Fn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "TaskDialogIndirect");
    if (!fn || FAILED(fn(&tc, nullptr, nullptr, &hide)))
        msg(std::wstring(DISCLAIMER_TITLE) + L"\n\n" + DISCLAIMER_TEXT, L"DIVA PSP Recomp", IDI_WARNING_);
    if (on_start && hide) { g_state[L"disclaimer_hide"] = L"1"; save_state(); }
}

// Design anwenden (Farben, Titelleiste, DarkMode_*-Themes, Menüs, Bilder)
void apply_theme() {
    const std::wstring t = g_state[L"theme"];
    g_dark = t == L"dark" || (t != L"light" && system_prefers_dark());
    g_bg = g_dark ? RGB(32, 32, 32) : RGB(255, 255, 255);
    g_fg = g_dark ? RGB(240, 240, 240) : RGB(0, 0, 0);
    if (g_bg_brush) DeleteObject(g_bg_brush);
    g_bg_brush = CreateSolidBrush(g_bg);
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(g_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    if (HMODULE ux = GetModuleHandleW(L"uxtheme.dll")) {   // dunkle Kontextmenüs
        using SetMode = int(WINAPI*)(int);
        using Flush = void(WINAPI*)();
        if (auto f = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135))) f(g_dark ? 2 : 0);
        if (auto f = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136))) f();
    }
    for (HWND c = GetWindow(g_wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[32];
        GetClassNameW(c, cls, 32);
        if (_wcsicmp(cls, L"ComboBox") == 0) SetWindowTheme(c, g_dark ? L"DarkMode_CFD" : nullptr, nullptr);
        else if (_wcsicmp(cls, L"Button") == 0 || _wcsicmp(cls, L"msctls_progress32") == 0)
            SetWindowTheme(c, g_dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    }
    const int k = g_dark ? 1 : 0;
    for (int i = 0; i < 3; i++) g_img_game[i] = g_img_dark[k][i];
    g_img_loading = g_img_dark[k][3];
    g_img_warning = g_img_dark[k][4];
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------------- Alpha-Testphase (T90)
std::wstring fmt_ymd(int d) {
    if (!d) return L"–";
    wchar_t b[16];
    swprintf(b, 16, L"%02d.%02d.%04d", d % 100, d / 100 % 100, d / 10000);
    return b;
}
std::wstring fmt_time(long long t) {
    if (!t) return T(L"nicht erreichbar", L"not reachable");
    const time_t tt = (time_t)t;
    tm l;
    localtime_s(&l, &tt);
    wchar_t b[32];
    wcsftime(b, 32, L"%d.%m.%Y %H:%M", &l);
    return b;
}
void alpha_expired_box() {
    msg(T(L"Die Alpha-Testphase für diese Version ist beendet.\n\nDanke fürs Testen! Bald wird der erste offizielle Release veröffentlicht.",
          L"The alpha test phase for this version has ended.\n\nThanks for testing! The first official release will be published soon."),
        T(L"Alpha-Testphase beendet", L"Alpha test phase ended"), IDI_WARNING_);
}

void show_test_page(bool on) {
    for (HWND h : g_set_ctl) ShowWindow(h, on ? SW_HIDE : SW_SHOW);
    for (HWND h : g_test_ctl) ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    if (!on) return;
    const std::wstring m = alpha_test_mode();
    const int cur = m == L"expired" ? 1 : m == L"tamper" ? 2 : 0;
    for (int i = 0; i < 3; i++) SendMessageW(g_ck_test[i], BM_SETCHECK, i == cur ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_test_status, T(L"Prüfe die echte Zeit (Zeitserver) …", L"Checking the real time (time server) …"));
    UpdateWindow(g_test_status);
    const alpha::Result ar = alpha::check(nullptr);
    const wchar_t* st = ar.state == alpha::State::NoLimit ? T(L"kein Ablauf (kein Alpha-Build)", L"no expiry (not an alpha build)")
                        : ar.state == alpha::State::Ok     ? T(L"gültig", L"valid")
                        : ar.state == alpha::State::Expired ? T(L"abgelaufen (Fall A)", L"expired (case A)")
                                                            : T(L"Uhr manipuliert (Fall B)", L"clock tampered (case B)");
    SetWindowTextW(g_test_status, (T(L"Alpha gültig bis: ", L"Alpha valid until: ") + fmt_ymd(ar.expires) + T(L"   (Build vom ", L"   (build from ") +
                                   fmt_ymd(ar.release) + L")\n" + T(L"Echte Zeit (Zeitserver): ", L"Real time (time server): ") + fmt_time(ar.real) +
                                   T(L"\nPC-Uhr: ", L"\nPC clock: ") + fmt_time(ar.local) + T(L"\nZustand jetzt: ", L"\nState now: ") + st).c_str());
}

std::wstring alpha_test_mode() { return g_state[L"alpha_test"]; }
const char* alpha_test_arg() {
    static std::string m;
    m = u8(alpha_test_mode());
    return m.c_str();
}

// Testszenario speichern, gilt bis wieder "Aktuell"
void save_test() {
    static const wchar_t* modes[] = {L"", L"expired", L"tamper"};
    int sc = 0;
    for (int i = 0; i < 3; i++) if (SendMessageW(g_ck_test[i], BM_GETCHECK, 0, 0) == BST_CHECKED) sc = i;
    if (sc) g_state[L"alpha_test"] = modes[sc]; else g_state.erase(L"alpha_test");
    save_state();
}

// ---------------------------------------------------------------- Updater (T87)
// prüft beim Start + alle 6 h die Releases in UPDATE_REPO; neuere Version = Box nur mit "Update installieren"
// Dateien (make_setup.py): DIVA-Update-<version>.zip + .sha256
// Ablauf: work\update, SHA-256, tar.exe, update.cmd ersetzt bin\ src\ (tools\ falls im Paket) + Exe, startet --updated
// danach Neubau der Spiele (built_<spiel> != Version); games\ saves\ config\ bleiben unberührt
struct SemVer { int v[3] = {}; std::string pre; bool ok = false; };

SemVer parse_ver(std::string s) {
    SemVer r;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    if (const size_t plus = s.find('+'); plus != std::string::npos) s.resize(plus);   // Build-Metadaten egal
    if (const size_t dash = s.find('-'); dash != std::string::npos) { r.pre = s.substr(dash + 1); s.resize(dash); }
    r.ok = sscanf(s.c_str(), "%d.%d.%d", &r.v[0], &r.v[1], &r.v[2]) == 3;
    // alpha.1 == alpha1
    std::string p;
    for (char c : r.pre) if (c != '.' && c != '-') p += (char)tolower((unsigned char)c);
    r.pre = p;
    return r;
}

// Vorabversionen: alpha2 < alpha10 < beta1; ohne Vorabversion ist neuer
int cmp_ver(const SemVer& a, const SemVer& b) {
    for (int i = 0; i < 3; i++) if (a.v[i] != b.v[i]) return a.v[i] < b.v[i] ? -1 : 1;
    if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : a.pre.empty() ? 1 : -1;
    size_t i = 0, j = 0;
    while (i < a.pre.size() && j < b.pre.size()) {
        if (isdigit((unsigned char)a.pre[i]) && isdigit((unsigned char)b.pre[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.pre.size() && isdigit((unsigned char)a.pre[i2])) i2++;
            while (j2 < b.pre.size() && isdigit((unsigned char)b.pre[j2])) j2++;
            const unsigned long long x = std::stoull(a.pre.substr(i, std::min<size_t>(i2 - i, 18)));
            const unsigned long long y = std::stoull(b.pre.substr(j, std::min<size_t>(j2 - j, 18)));
            if (x != y) return x < y ? -1 : 1;
            i = i2;
            j = j2;
        } else {
            if (a.pre[i] != b.pre[j]) return a.pre[i] < b.pre[j] ? -1 : 1;
            i++;
            j++;
        }
    }
    return i < a.pre.size() ? 1 : j < b.pre.size() ? -1 : 0;
}

// HTTPS-GET (WinHTTP); mit file: in die Datei, sonst Text in body
bool http_get(const std::wstring& url, std::string* body, const fs::path* file, DWORD timeout_ms, bool github_api) {
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[2048], extra[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    const std::wstring hostname(host, uc.dwHostNameLength), object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"DIVA-PSP-Recomp-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"DIVA-PSP-Recomp-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, hostname.c_str(), uc.nPort, 0) : nullptr;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        WinHttpSetTimeouts(ses, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms);
        if (github_api)
            WinHttpAddRequestHeaders(req, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28", (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        DWORD status = 0, len = sizeof status;
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            unsigned long long total = 0, got = 0;
            wchar_t cl[32] = {};
            DWORD cll = sizeof cl;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &cll, WINHTTP_NO_HEADER_INDEX))
                total = _wcstoui64(cl, nullptr, 10);
            std::ofstream out;
            if (file) out.open(*file, std::ios::binary);
            std::vector<char> buf(1 << 16);
            int last_pct = -1;
            ok = !file || (bool)out;
            for (DWORD n = 0; ok && WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &n) && n;) {
                if (file) out.write(buf.data(), n);
                else body->append(buf.data(), n);
                got += n;
                if (file && total) {
                    const int pct = (int)(got * 100 / total);
                    if (pct != last_pct) {
                        last_pct = pct;
                        post_status(T(L"Update wird geladen … ", L"Downloading update … ") + std::to_wstring(pct) + L" %");
                    }
                }
            }
            if (file) { out.close(); ok = ok && (bool)out; }
            ok = ok && (!total || got == total);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

// "key":"wert" ab Stelle p; reicht für die GitHub-Releaseliste
std::string json_value(const std::string& j, size_t from, const char* key, size_t* at = nullptr) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return {};
    if (at) *at = p;
    p += k.size();
    while (p < j.size() && (j[p] == ' ' || j[p] == ':' || j[p] == '\n' || j[p] == '\r' || j[p] == '\t')) p++;
    if (p >= j.size()) return {};
    if (j[p] != '"') {   // true/false/Zahl
        size_t e = p;
        while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != ']') e++;
        return j.substr(p, e - p);
    }
    std::string v;
    for (p++; p < j.size() && j[p] != '"'; p++) {
        if (j[p] == '\\' && p + 1 < j.size()) p++;   // \/ \" \\ (kein \u in Tags/URLs)
        v += j[p];
    }
    return v;
}

struct UpdateInfo { std::string tag, zip_url, sha_url; };
std::atomic<bool> g_update_checking{false}, g_update_shown{false};
UpdateInfo* g_update_pending = nullptr;   // erst nach dem Bau zeigen

// neuere Version mit beiden Update-Dateien suchen
bool find_update(UpdateInfo& out) {
    std::string j;
    // DIVA_UPDATE_API nur für Tests (lokaler Server)
    wchar_t api[256] = L"https://api.github.com";
    if (const DWORD n = GetEnvironmentVariableW(L"DIVA_UPDATE_API", api, 256); n == 0 || n >= 256) wcscpy_s(api, L"https://api.github.com");
    if (!http_get(std::wstring(api) + L"/repos/" + UPDATE_REPO + L"/releases?per_page=30", &j, nullptr, 4000, true)) return false;
    SemVer best = parse_ver(u8(VERSION));
    bool found = false;
    size_t pos = 0;
    while (true) {
        size_t at = 0;
        const std::string tag = json_value(j, pos, "tag_name", &at);
        if (tag.empty()) break;
        size_t next = j.find("\"tag_name\"", at + 10);
        const std::string seg = j.substr(at, next == std::string::npos ? std::string::npos : next - at);
        pos = at + 10;
        if (json_value(seg, 0, "draft") == "true") continue;
        const SemVer v = parse_ver(tag);
        if (!v.ok || cmp_ver(v, best) <= 0) continue;
        std::string zip, sha;
        for (size_t a = 0;;) {
            size_t where = 0;
            const std::string u = json_value(seg, a, "browser_download_url", &where);
            if (u.empty()) break;
            a = where + 20;
            const std::string f = u.substr(u.rfind('/') + 1);
            auto ends = [&](const char* e) { const size_t n = strlen(e); return f.size() >= n && f.compare(f.size() - n, n, e) == 0; };
            if (f.rfind("DIVA-Update-", 0) != 0) continue;
            if (ends(".zip.sha256")) sha = u;
            else if (ends(".zip")) zip = u;
        }
        if (zip.empty() || sha.empty()) continue;
        best = v;
        out = {tag, zip, sha};
        found = true;
    }
    return found;
}

void update_check_async() {
    if (!*UPDATE_REPO || g_update_shown || g_update_checking.exchange(true)) return;
    std::thread([] {
        auto* u = new UpdateInfo;
        if (find_update(*u)) PostMessageW(g_wnd, WM_UPDATE_FOUND, 0, (LPARAM)u);
        else delete u;
        g_update_checking = false;
    }).detach();
}

std::string sha256_file(const fs::path& p) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream f(p, std::ios::binary);
        std::vector<char> b(1 << 20);
        while (f) {
            f.read(b.data(), (std::streamsize)b.size());
            if (f.gcount() > 0) BCryptHashData(h, (PUCHAR)b.data(), (ULONG)f.gcount(), 0);
        }
        unsigned char d[32];
        if (BCryptFinishHash(h, d, 32, 0) == 0)
            for (unsigned char c : d) { char x[3]; snprintf(x, 3, "%02x", c); hex += x; }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

fs::path update_dir() { return L.data / L"work" / L"update"; }

// laden, prüfen, entpacken, update.cmd schreiben (Thread); leer = bereit
std::wstring prepare_update(const UpdateInfo& u) {
    std::error_code ec;
    const fs::path dir = update_dir();
    fs::remove_all(dir, ec);
    fs::create_directories(dir / L"new", ec);
    const fs::path zip = dir / L"update.zip";
    std::string sha;
    if (!http_get(fs::path(std::u8string(u.sha_url.begin(), u.sha_url.end())).wstring(), &sha, nullptr, 15000, false) ||
        !http_get(fs::path(std::u8string(u.zip_url.begin(), u.zip_url.end())).wstring(), nullptr, &zip, 30000, false))
        return T(L"Download fehlgeschlagen.", L"Download failed.");
    post_status(T(L"Update wird geprüft …", L"Checking update …"));
    sha = sha.substr(0, std::min<size_t>(sha.find_first_of(" \t\r\n"), 64));
    for (char& c : sha) c = (char)tolower((unsigned char)c);
    if (sha.size() != 64 || sha != sha256_file(zip)) return T(L"Die Prüfsumme stimmt nicht (Download beschädigt).", L"Checksum mismatch (download damaged).");
    post_status(T(L"Update wird entpackt …", L"Unpacking update …"));
    if (run_hidden(q(tar_exe()) + L" -x -f update.zip -C new", dir, 600000) != 0 || !fs::exists(dir / L"new" / L"DIVA Launcher.exe", ec))
        return T(L"Das Update konnte nicht entpackt werden.", L"The update could not be unpacked.");
    fs::remove(zip, ec);
    // Skript nur ASCII, Pfade über %LOCALAPPDATA%; Tools mit vollem System32-Pfad (sonst evtl. find aus Git)
    std::ofstream c(dir / L"update.cmd", std::ios::binary);
    c << "@echo off\r\n"
         "rem DIVA PSP Recomp: Update einspielen (geschrieben vom Launcher, T87)\r\n"
         "set \"DIVA_DIR=%LOCALAPPDATA%\\DIVA PSP Recomp\"\r\n"
         "set \"DIVA_NEW=%DIVA_DIR%\\work\\update\\new\"\r\n"
         "set \"DIVA_LOG=%DIVA_DIR%\\work\\update-log.txt\"\r\n"
         "set \"SYS=%SystemRoot%\\System32\"\r\n"
         "echo Update auf " << u.tag << " > \"%DIVA_LOG%\"\r\n"
         "set /a DIVA_WAIT=0\r\n"
         ":wait\r\n"
         "\"%SYS%\\tasklist.exe\" /FI \"PID eq " << GetCurrentProcessId() << "\" /NH 2>nul | \"%SYS%\\find.exe\" /I \".exe\" >nul || goto go\r\n"
         "set /a DIVA_WAIT+=1\r\n"
         "if %DIVA_WAIT% geq 60 goto go\r\n"
         "\"%SYS%\\ping.exe\" -n 2 127.0.0.1 >nul\r\n"
         "goto wait\r\n"
         ":go\r\n"
         "\"%SYS%\\robocopy.exe\" \"%DIVA_NEW%\\bin\" \"%DIVA_DIR%\\bin\" /MIR /R:10 /W:1 /NP /LOG+:\"%DIVA_LOG%\" >nul\r\n"
         "\"%SYS%\\robocopy.exe\" \"%DIVA_NEW%\\src\" \"%DIVA_DIR%\\src\" /MIR /R:10 /W:1 /NP /NFL /LOG+:\"%DIVA_LOG%\" >nul\r\n"
         "if exist \"%DIVA_NEW%\\tools\" \"%SYS%\\robocopy.exe\" \"%DIVA_NEW%\\tools\" \"%DIVA_DIR%\\tools\" /MIR /R:10 /W:1 /NP /NFL /LOG+:\"%DIVA_LOG%\" >nul\r\n"
         "set /a DIVA_TRY=0\r\n"
         ":exe\r\n"
         "copy /y \"%DIVA_NEW%\\DIVA Launcher.exe\" \"%DIVA_DIR%\\DIVA Launcher.exe\" >>\"%DIVA_LOG%\" 2>&1 && goto start\r\n"
         "set /a DIVA_TRY+=1\r\n"
         "if %DIVA_TRY% geq 20 goto start\r\n"
         "\"%SYS%\\ping.exe\" -n 2 127.0.0.1 >nul\r\n"
         "goto exe\r\n"
         ":start\r\n"
         "echo fertig >> \"%DIVA_LOG%\"\r\n"
         "start \"\" \"%DIVA_DIR%\\DIVA Launcher.exe\" --updated\r\n";
    c.close();
    if (!c) return T(L"Das Update-Skript konnte nicht geschrieben werden.", L"The update script could not be written.");
    return {};
}

// Pflicht-Box, kein Abbrechen
void show_update_box(UpdateInfo* u) {
    std::unique_ptr<UpdateInfo> own(u);
    if (g_update_shown.exchange(true)) return;
    const std::wstring tag = fs::path(std::u8string(u->tag.begin(), u->tag.end())).wstring();
    if (!is_installed()) {   // Entwickler-Build: nur Hinweis
        SetWindowTextW(g_status, (T(L"Neue Version im Release-Repo: ", L"New version in the release repo: ") + tag +
                                  T(L" (Entwickler-Build: kein automatisches Update)", L" (developer build: no automatic update)")).c_str());
        return;
    }
    const std::wstring text = T(L"Neue Version: ", L"New version: ") + tag + T(L"  (installiert: ", L"  (installed: ") + VERSION + L")\n\n" +
                              T(L"Das Update ist für die Alpha nötig und dauert meist nur ein paar Minuten. Danach wird das Spiel neu gebaut. "
                                L"Deine Spielstände und Einstellungen bleiben erhalten.",
                                L"This update is required for the alpha and usually takes only a few minutes. Afterwards the game is rebuilt. "
                                L"Your save data and settings are kept.");
    TASKDIALOG_BUTTON btn = {1000, T(L"Update installieren", L"Install update")};
    TASKDIALOGCONFIG tc = {sizeof tc};
    tc.hwndParent = g_wnd;
    tc.hInstance = GetModuleHandleW(nullptr);
    tc.dwFlags = TDF_USE_HICON_MAIN | TDF_POSITION_RELATIVE_TO_WINDOW;   // kein X, kein Esc
    tc.pszWindowTitle = L"DIVA PSP Recomp";
    tc.hMainIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_SUCCESS), IMAGE_ICON, 32, 32, 0);
    tc.pszMainInstruction = T(L"Update verfügbar", L"Update available");
    tc.pszContent = text.c_str();
    tc.cButtons = 1;
    tc.pButtons = &btn;
    using Fn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "TaskDialogIndirect");
    if (!fn || FAILED(fn(&tc, nullptr, nullptr, nullptr))) msg(text, T(L"Update verfügbar", L"Update available"), IDI_SUCCESS);
    // Launcher gesperrt, Fortschritt in der Statuszeile
    g_busy = true;
    refresh_tiles();
    show_page(false);
    SendMessageW(g_progress, PBM_SETMARQUEE, TRUE, 30);
    ShowWindow(g_progress, SW_SHOW);
    g_state[L"update_target"] = tag;
    save_state();
    std::thread([info = *u] {
        const std::wstring err = prepare_update(info);
        PostMessageW(g_wnd, WM_UPDATE_DONE, 0, (LPARAM) new std::wstring(err));
    }).detach();
}

// Spiele mit anderer Bau-Version neu bauen (gemerkte ISO)
void rebuild_outdated() {
    if (g_busy) return;
    std::error_code ec;
    for (int i = 0; i < 3; i++) {
        if (!GAMES[i].supported || !game_ready(i) || g_state[std::wstring(L"built_") + GAMES[i].key] == VERSION) continue;
        const std::wstring iso = game_iso(i);
        msg(std::wstring(GAMES[i].title) + T(L" wurde mit einer älteren Version gebaut und wird jetzt neu gebaut (dauert ein paar Minuten).",
                                             L" was built with an older version and is rebuilt now (takes a few minutes)."),
            L"DIVA PSP Recomp", IDI_WARNING_);
        if (!iso.empty() && fs::exists(iso, ec)) start_build(i, iso);
        else {
            msg(T(L"Die ISO wurde verschoben oder gelöscht. Bitte wähle sie neu aus.", L"The ISO was moved or deleted. Please select it again."),
                L"DIVA PSP Recomp", IDI_ERROR_);
            pick_iso();
        }
        return;   // eins nach dem anderen (WM_BUILD_DONE)
    }
}

// ---------------------------------------------------------------- Warnung zum Bastler-Schalter
// Bestätigen erst nach Eintippen von RISIKO / RISK, Knopf dann rot
struct RiskDlg { HWND edit, ok; bool armed, yes, done; HFONT head; HBRUSH edit_bg; };
RiskDlg g_risk;

LRESULT CALLBACK risk_proc(HWND w, UINT um, WPARAM wp, LPARAM lp) {
    RiskDlg& d = g_risk;
    switch (um) {
    case WM_COMMAND:
        if (LOWORD(wp) == 3 && HIWORD(wp) == EN_CHANGE) {
            wchar_t b[32] = {};
            GetWindowTextW(d.edit, b, 32);
            d.armed = _wcsicmp(b, T(L"RISIKO", L"RISK")) == 0;
            EnableWindow(d.ok, d.armed);
            InvalidateRect(d.ok, nullptr, TRUE);
        } else if (LOWORD(wp) == IDOK && d.armed) { d.yes = true; DestroyWindow(w); }
        else if (LOWORD(wp) == IDCANCEL) DestroyWindow(w);
        return 0;
    case WM_DRAWITEM: {   // grau, bis das Wort stimmt, dann rot
        auto* di = (DRAWITEMSTRUCT*)lp;
        const bool down = di->itemState & ODS_SELECTED;
        const COLORREF bg = !d.armed ? (g_dark ? RGB(70, 70, 70) : RGB(204, 204, 204)) : down ? RGB(150, 25, 15) : RGB(196, 43, 28);
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(di->hDC, &di->rcItem, br);
        DeleteObject(br);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, d.armed ? RGB(255, 255, 255) : RGB(130, 130, 130));
        SelectObject(di->hDC, g_font);
        wchar_t t[64];
        GetWindowTextW(di->hwndItem, t, 64);
        DrawTextW(di->hDC, t, -1, &di->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (d.armed && (di->itemState & ODS_FOCUS)) { RECT f = di->rcItem; InflateRect(&f, -3, -3); DrawFocusRect(di->hDC, &f); }
        return TRUE;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, g_fg);
        SetBkColor((HDC)wp, g_dark ? RGB(45, 45, 45) : RGB(255, 255, 255));
        return (LRESULT)d.edit_bg;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, g_fg);
        return (LRESULT)g_bg_brush;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(w, &rc);
        FillRect((HDC)wp, &rc, g_bg_brush);
        return 1;
    }
    case WM_CLOSE: DestroyWindow(w); return 0;
    case WM_DESTROY: d.done = true; return 0;
    }
    return DefWindowProcW(w, um, wp, lp);
}

bool risk_dialog() {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = risk_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"DivaRisk";
        RegisterClassW(&wc);
        reg = true;
    }
    RiskDlg& d = g_risk;
    d = {};
    d.head = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    d.edit_bg = CreateSolidBrush(g_dark ? RGB(45, 45, 45) : RGB(255, 255, 255));
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r = {0, 0, 580, 350};
    AdjustWindowRect(&r, style, FALSE);
    RECT o;
    GetWindowRect(g_wnd, &o);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DivaRisk", T(L"Achtung", L"Attention"), style, (o.left + o.right - ww) / 2,
                             (o.top + o.bottom - wh) / 2, ww, wh, g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    auto ctl = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cw, int ch, int id, HFONT f) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, x, y, cw, ch, w, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return c;
    };
    HWND icon = ctl(L"STATIC", L"", SS_ICON, 20, 22, 32, 32, 0, g_font);
    SendMessageW(icon, STM_SETICON, (WPARAM)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_WARNING_), IMAGE_ICON, 32, 32, 0), 0);
    ctl(L"STATIC", T(L"AUF EIGENES RISIKO", L"AT YOUR OWN RISK"), 0, 68, 22, 490, 32, 0, d.head);
    ctl(L"STATIC", T(L"Damit baut der Launcher auch eine Fassung des Spiels, die er nicht kennt (andere Version, Region oder "
                     L"anderer Patch). Das ist nur für Bastler gedacht.\n\n"
                     L"Mögliche Folgen: Das Spiel stürzt ab, verhält sich falsch oder beschädigt deine Spielstände. "
                     L"Bitte melde Fehler nur mit einer unterstützten Fassung.\n\n"
                     L"Sichere vorher deine Spielstände. Zum Einschalten tippe RISIKO ein:",
                     L"With this, the launcher also builds a version of the game it doesn't know (a different version, region "
                     L"or patch). This is only meant for tinkerers.\n\n"
                     L"What can happen: the game crashes, behaves wrongly or damages your save data. "
                     L"Please only report bugs with a supported version.\n\n"
                     L"Back up your save data first. To turn it on, type RISK:"),
        0, 68, 62, 490, 176, 0, g_font);
    d.edit = ctl(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 68, 244, 220, 28, 3, g_font);
    d.ok = ctl(L"BUTTON", T(L"Ja, auf eigenes Risiko", L"Yes, at my own risk"), BS_OWNERDRAW | WS_TABSTOP | WS_DISABLED, 68, 296, 220, 36, IDOK, g_font);
    HWND cancel = ctl(L"BUTTON", T(L"Abbrechen", L"Cancel"), WS_TABSTOP, 438, 296, 120, 36, IDCANCEL, g_font);
    if (g_dark) {
        SetWindowTheme(cancel, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(d.edit, L"DarkMode_CFD", nullptr);
    }
    EnableWindow(g_wnd, FALSE);
    ShowWindow(w, SW_SHOW);
    SetFocus(d.edit);
    MSG m;
    while (!d.done) {
        if (GetMessageW(&m, nullptr, 0, 0) <= 0) { PostQuitMessage((int)m.wParam); break; }
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    EnableWindow(g_wnd, TRUE);
    SetForegroundWindow(g_wnd);
    DeleteObject(d.head);
    DeleteObject(d.edit_bg);
    return d.yes;
}

// ---------------------------------------------------------------- Aufbau
HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font, std::vector<HWND>* page) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    if (page) page->push_back(c);
    return c;
}

HWND mk_combo(const wchar_t* label, int y, const wchar_t* const* items, int n, int id) {
    mk(L"STATIC", label, 0, 40, y + 4, 200, 24, 0, g_font, &g_set_ctl);
    HWND cb = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 250, y, 420, 300, id, g_font, &g_set_ctl);
    for (int i = 0; i < n; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)items[i]);
    return cb;
}

enum { ID_TILE = 100, ID_SETTINGS = 200, ID_SAVES, ID_REPORT, ID_ABOUT, ID_SAVE, ID_BACK, ID_XBRZ, ID_SCALE, ID_XBRZ_LVL,
       ID_UNINST_GAME, ID_DEL_SAVES, ID_UNINST_ALL, ID_THEME, ID_XBRZ_LABEL, ID_TESTS, ID_TEST_A, ID_TEST_B, ID_TEST_C,
       ID_TEST_RUN, ID_TEST_BACK, ID_LANG, ID_LAT, ID_LAT_LABEL, ID_LAT_RESET, ID_UNKNOWN, ID_UNKNOWN_LABEL, ID_SOON_R, ID_SOON_S, ID_SOON_F };

// Steuerelemente anlegen (auch nach Sprachwechsel, vorher destroy_ui)
void build_ui() {
    if (!g_font) {
        g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_big = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_tile = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_icon = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
    }
    const std::wstring title = L"DIVA PSP Recomp   " + std::wstring(VERSION);
    g_title = mk(L"STATIC", title.c_str(), WS_VISIBLE, 30, 20, 700, 40, 0, g_font_big, nullptr);
    for (int i = 0; i < 1; i++) {   // Alpha: nur 1st
        const int x = 270;
        mk(L"BUTTON", L"", BS_GROUPBOX | WS_VISIBLE, x, 74, 220, 236, 0, g_font, &g_main_ctl);
        g_tile_name[i] = mk(L"STATIC", GAMES[i].title, WS_VISIBLE | SS_CENTER, x + 10, 170, 200, 28, 0, g_font_tile, &g_main_ctl);
        if (GAMES[i].supported) g_tile_state[i] = mk(L"STATIC", L"", WS_VISIBLE | SS_CENTER, x + 10, 204, 200, 24, 0, g_font, &g_main_ctl);
        else g_tile_state[i] = mk(L"STATIC", L"", WS_VISIBLE | SS_LEFT, x + 78, 204, 132, 24, 0, g_font, &g_main_ctl);   // Warnsymbol
        g_tile_btn[i] = mk(L"BUTTON", L"", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x + 25, 250, 170, 44, ID_TILE + i, g_font_tile, &g_main_ctl);
    }
    g_status = mk(L"STATIC", T(L"Tipp: ISO-Datei einfach ins Fenster ziehen.", L"Tip: just drag the ISO file into this window."), WS_VISIBLE,
                  70, 326, 660, 44, 0, g_font, &g_main_ctl);
    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_MARQUEE, 30, 374, 700, 14, g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_btn_settings = mk(L"BUTTON", T(L"Einstellungen", L"Settings"), WS_VISIBLE | WS_TABSTOP, 200, 410, 170, 36, ID_SETTINGS, g_font, &g_main_ctl);
    g_btn_report = mk(L"BUTTON", T(L"Fehler melden", L"Report a bug"), WS_VISIBLE | WS_TABSTOP, 390, 410, 170, 36, ID_REPORT, g_font, &g_main_ctl);

    // Seite Einstellungen (wie im Hauptprojekt; Grafik kommt erst per Update)
    mk(L"STATIC", T(L"Einstellungen (gelten für alle Spiele)", L"Settings (apply to all games)"), 0, 30, 75, 600, 30, 0, g_font_tile, &g_set_ctl);
    // "(bald)": Liste lässt sich aufklappen, die Auswahl springt aber zurück (ID_SOON_*)
    const wchar_t* renderers[] = {T(L"Software (CPU, GDI)", L"Software (CPU, GDI)"), T(L"Direct3D 11 (Grafikkarte)", L"Direct3D 11 (graphics card)"),
                                  T(L"Vulkan", L"Vulkan")};
    SendMessageW(mk_combo(T(L"Renderer (bald)", L"Renderer (soon)"), 110, renderers, 3, ID_SOON_R), CB_SETCURSEL, 0, 0);
    const wchar_t* scales[] = {L"1×  480×272 (Original)", L"2×  960×544", L"3×  1440×816", L"4×  1920×1088 (1080p)", L"6×  2880×1632 (1440p)",
                               L"8×  3840×2176 (4K)"};
    SendMessageW(mk_combo(T(L"Auflösung (intern) (bald)", L"Resolution (internal) (soon)"), 142, scales, 6, ID_SOON_S), CB_SETCURSEL, 0, 0);
    const wchar_t* fps_names[] = {L"30 (Original)", L"60", L"120", L"240"};
    SendMessageW(mk_combo(T(L"Bilder pro Sekunde (bald)", L"Frames per second (soon)"), 174, fps_names, 4, ID_SOON_F), CB_SETCURSEL, 0, 0);
    mk(L"BUTTON", L"", BS_CHECKBOX | WS_DISABLED, 40, 210, 18, 24, 0, g_font, &g_set_ctl);
    mk(L"STATIC", T(L"Texturfilter xBRZ (bald)", L"Texture filter xBRZ (soon)"), 0, 62, 212, 186, 24, 0, g_font, &g_set_ctl);
    const wchar_t* themes[] = {T(L"Wie Windows", L"Like Windows"), T(L"Hell", L"Light"), T(L"Dunkel", L"Dark")};
    g_cb_theme = mk_combo(T(L"Design (Launcher)", L"Theme (launcher)"), 238, themes, 3, ID_THEME);
    const wchar_t* langs[] = {L"Deutsch", L"English"};
    g_cb_lang = mk_combo(T(L"Sprache", L"Language"), 270, langs, 2, ID_LANG);
    g_ck_unknown = mk(L"BUTTON", L"", BS_CHECKBOX | WS_TABSTOP, 40, 308, 18, 24, ID_UNKNOWN, g_font, &g_set_ctl);
    mk(L"STATIC", T(L"Unbekannte Fassung trotzdem bauen (auf eigenes Risiko)", L"Build an unknown version anyway (at your own risk)"),
       SS_NOTIFY, 62, 310, 600, 24, ID_UNKNOWN_LABEL, g_font, &g_set_ctl);
    mk(L"STATIC", T(L"Deinstallieren", L"Uninstall"), 0, 30, 338, 300, 26, 0, g_font_tile, &g_set_ctl);
    g_btn_uninst_game = mk(L"BUTTON", T(L"Spiel deinstallieren", L"Uninstall game"), WS_TABSTOP, 40, 370, 220, 30, ID_UNINST_GAME, g_font, &g_set_ctl);
    g_btn_uninst_all = mk(L"BUTTON", T(L"Alles deinstallieren", L"Uninstall everything"), WS_TABSTOP, 270, 370, 220, 30, ID_UNINST_ALL, g_font, &g_set_ctl);
    g_btn_save = mk(L"BUTTON", T(L"Speichern", L"Save"), WS_TABSTOP | BS_DEFPUSHBUTTON, 30, 416, 160, 36, ID_SAVE, g_font, &g_set_ctl);
    g_btn_back = mk(L"BUTTON", T(L"Zurück", L"Back"), WS_TABSTOP, 210, 416, 160, 36, ID_BACK, g_font, &g_set_ctl);
    // Testseite nur im Entwickler-Build
    if (!is_installed()) mk(L"BUTTON", L"Tests …", WS_TABSTOP, 570, 416, 160, 36, ID_TESTS, g_font, &g_set_ctl);

    // Seite Tests (T90)
    mk(L"STATIC", T(L"Tests: Alpha-Testphase", L"Tests: alpha test phase"), 0, 30, 75, 600, 30, 0, g_font_tile, &g_test_ctl);
    g_test_status = mk(L"STATIC", L"", 0, 40, 112, 690, 92, 0, g_font, &g_test_ctl);
    const wchar_t* tests[] = {T(L"Aktueller Zustand (echte Prüfung)", L"Current state (real check)"),
                              T(L"Testphase abgelaufen (Fall A: Ende-Fenster)", L"Test phase expired (case A: end window)"),
                              T(L"Ablaufdatum / Uhr umgeändert (Fall B: Grafikfehler)", L"Expiry date / clock changed (case B: graphics error)")};
    for (int i = 0; i < 3; i++) {
        g_ck_test[i] = mk(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, 40, 222 + i * 34, 18, 24, ID_TEST_A + i, g_font, &g_test_ctl);
        mk(L"STATIC", tests[i], SS_NOTIFY, 62, 224 + i * 34, 600, 24, ID_TEST_A + i + 100, g_font, &g_test_ctl);
    }
    SendMessageW(g_ck_test[0], BM_SETCHECK, BST_CHECKED, 0);
    mk(L"STATIC", T(L"Gespeicherter Fall gilt beim Start des Launchers und bei \"Spielen\", bis wieder \"Aktuell\" gewählt ist.",
                    L"The saved case applies when the launcher starts and on \"Play\" until \"Current\" is selected again."),
       0, 40, 330, 690, 24, 0, g_font, &g_test_ctl);
    mk(L"BUTTON", T(L"Speichern", L"Save"), WS_TABSTOP | BS_DEFPUSHBUTTON, 30, 416, 160, 36, ID_TEST_RUN, g_font, &g_test_ctl);
    mk(L"BUTTON", T(L"Zurück", L"Back"), WS_TABSTOP, 210, 416, 160, 36, ID_TEST_BACK, g_font, &g_test_ctl);
    refresh_tiles();
}

// Steuerelemente entfernen, danach build_ui + apply_theme
void destroy_ui() {
    while (HWND c = GetWindow(g_wnd, GW_CHILD)) DestroyWindow(c);
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }   // Popup, kein Kind
    g_main_ctl.clear();
    g_set_ctl.clear();
    g_test_ctl.clear();
}

bool g_settings_page = false;

void paint(HDC dc) {
    Gdiplus::Graphics g(dc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    if (!g_settings_page) {
        for (int i = 0; i < 3; i++) {
            if (!g_img_game[i]) continue;
            const int x = 30 + i * 240 + 110 - 40;
            if (GAMES[i].supported) g.DrawImage(g_img_game[i], x, 92, 80, 80);
            else {   // noch nicht verfügbar: blass
                Gdiplus::ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, 0.3f, 0}, {0, 0, 0, 0, 1}}};
                Gdiplus::ImageAttributes ia;
                ia.SetColorMatrix(&cm);
                g.DrawImage(g_img_game[i], Gdiplus::Rect(x, 92, 80, 80), 0, 0, 64, 64, Gdiplus::UnitPixel, &ia);
                if (g_img_warning) g.DrawImage(g_img_warning, 30 + i * 240 + 52, 204, 22, 22);
            }
        }
        if (g_busy && g_img_loading) g.DrawImage(g_img_loading, 30, 326, 30, 30);
    }
}

LRESULT CALLBACK wnd_proc(HWND w, UINT um, WPARAM wp, LPARAM lp) {
    switch (um) {
    case WM_DRAWITEM: {   // schwarzer Knopf
        auto* d = (DRAWITEMSTRUCT*)lp;
        if (d->CtlID != ID_UNINST_ALL) break;
        const bool down = d->itemState & ODS_SELECTED, focus = d->itemState & ODS_FOCUS;
        HBRUSH bg = CreateSolidBrush(down ? RGB(60, 60, 60) : RGB(0, 0, 0));
        FillRect(d->hDC, &d->rcItem, bg);
        DeleteObject(bg);
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, RGB(255, 255, 255));
        SelectObject(d->hDC, g_font);
        wchar_t t[64];
        GetWindowTextW(d->hwndItem, t, 64);
        DrawTextW(d->hDC, t, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (g_dark) {   // dunkel: heller Rand
            HBRUSH frame = CreateSolidBrush(RGB(200, 200, 200));
            FrameRect(d->hDC, &d->rcItem, frame);
            DeleteObject(frame);
        }
        if (focus) { RECT f = d->rcItem; InflateRect(&f, -3, -3); DrawFocusRect(d->hDC, &f); }
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        paint(dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(w, &rc);
        FillRect((HDC)wp, &rc, g_bg_brush);
        return 1;
    }
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)lp;
        if (nm->hwndFrom == g_tip && nm->code == TTN_GETDISPINFOW && nm->idFrom == TIP_LAT_HELP) {
            ((NMTTDISPINFOW*)lp)->lpszText = (LPWSTR)g_lat_help;
            return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, g_fg);
        SetBkColor((HDC)wp, g_bg);
        return (LRESULT)g_bg_brush;
    case WM_SETTINGCHANGE:   // hell/dunkel
        if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) apply_theme();
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= ID_TILE && id < ID_TILE + 3) {
            const int gi = id - ID_TILE;
            if (game_ready(gi)) play(gi); else pick_iso();
        } else if (id == ID_SETTINGS) show_page(true);
        else if (id == ID_BACK) show_page(false);
        else if (id == ID_SAVE) { save_settings_ui(); show_page(false); SetWindowTextW(g_status, T(L"Einstellungen gespeichert.", L"Settings saved.")); }
        else if (id == ID_XBRZ) {
            g_xbrz_touched = true;
            EnableWindow(g_cb_xbrz_lvl, SendMessageW(g_ck_xbrz, BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (id == ID_XBRZ_LVL && HIWORD(wp) == CBN_SELCHANGE) g_xbrz_touched = true;
        else if (id == ID_SCALE && HIWORD(wp) == CBN_SELCHANGE && !g_xbrz_touched) xbrz_auto();   // xBRZ folgt Auflösung
        else if (id == ID_LANG && HIWORD(wp) == CBN_SELCHANGE) {   // Sprache sofort, Seite neu aufbauen
            const bool en = SendMessageW(g_cb_lang, CB_GETCURSEL, 0, 0) == 1;
            if (en != g_en) {
                save_settings_ui();
                g_en = en;
                g_state[L"lang"] = en ? L"en" : L"de";
                save_state();
                set_setting("lang", en ? "en" : "de");
                destroy_ui();
                build_ui();
                apply_theme();
                show_page(true);
            }
        } else if (id == ID_THEME && HIWORD(wp) == CBN_SELCHANGE) {   // Design sofort
            static const wchar_t* keys[] = {L"system", L"light", L"dark"};
            g_state[L"theme"] = keys[std::clamp((int)SendMessageW(g_cb_theme, CB_GETCURSEL, 0, 0), 0, 2)];
            save_state();
            apply_theme();
        } else if (id == ID_LAT_RESET) {
            SetWindowTextW(g_ed_latency, L"");   // leer = Standard
        } else if (id == ID_LAT) {
            const bool on = SendMessageW(g_ck_latency, BM_GETCHECK, 0, 0) == BST_CHECKED;
            EnableWindow(g_ed_latency, on);
            EnableWindow(g_btn_lat_reset, on);
        } else if (id == ID_LAT_LABEL && HIWORD(wp) == STN_CLICKED) {
            SendMessageW(g_ck_latency, BM_SETCHECK, SendMessageW(g_ck_latency, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            SendMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(ID_LAT, BN_CLICKED), (LPARAM)g_ck_latency);
        } else if (id == ID_XBRZ_LABEL && HIWORD(wp) == STN_CLICKED) {
            SendMessageW(g_ck_xbrz, BM_SETCHECK, SendMessageW(g_ck_xbrz, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            SendMessageW(g_wnd, WM_COMMAND, MAKEWPARAM(ID_XBRZ, BN_CLICKED), (LPARAM)g_ck_xbrz);
        }
        else if (id >= ID_SOON_R && id <= ID_SOON_F && HIWORD(wp) == CBN_SELCHANGE)   // "(bald)": bleibt beim Ist-Zustand
            SendMessageW((HWND)lp, CB_SETCURSEL, 0, 0);
        else if (id == ID_UNKNOWN || (id == ID_UNKNOWN_LABEL && HIWORD(wp) == STN_CLICKED)) {   // Bastler-Schalter
            const bool on = g_state[L"unknown_version"] != L"1" && risk_dialog();
            if (on) g_state[L"unknown_version"] = L"1"; else g_state.erase(L"unknown_version");
            save_state();
            SendMessageW(g_ck_unknown, BM_SETCHECK, on, 0);
        }
        else if (id == ID_TESTS) show_test_page(true);
        else if (id == ID_TEST_BACK) show_test_page(false);
        else if (id == ID_TEST_RUN) { save_test(); show_test_page(false); SetWindowTextW(g_status, T(L"Testszenario gespeichert.", L"Test scenario saved.")); }
        else if ((id >= ID_TEST_A && id <= ID_TEST_C) || (id >= ID_TEST_A + 100 && id <= ID_TEST_C + 100)) {   // nur ein Szenario
            const int k = (id >= ID_TEST_A + 100 ? id - 100 : id) - ID_TEST_A;
            for (int i = 0; i < 3; i++) SendMessageW(g_ck_test[i], BM_SETCHECK, i == k ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        else if (id == ID_UNINST_GAME) uninstall_game();
        else if (id == ID_DEL_SAVES) delete_saves();
        else if (id == ID_UNINST_ALL) uninstall_all();
        else if (id == ID_SAVES) ShellExecuteW(w, L"open", L.saves.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (id == ID_REPORT) report_bug();
        else if (id == ID_ABOUT) about();
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t file[MAX_PATH];
        if (DragQueryFileW((HDROP)wp, 0, file, MAX_PATH)) add_iso(file);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_STATUS: {
        auto* s = (std::wstring*)lp;
        SetWindowTextW(g_status, s->c_str());
        delete s;
        return 0;
    }
    case WM_GAME_FAILED: {   // Spiel sofort beendet: Grund zeigen
        auto* t = (std::wstring*)lp;
        msg(T(L"Das Spiel hat sich sofort beendet (Code ", L"The game closed right away (code ") + std::to_wstring((unsigned)wp) + L").\n\n" + *t,
            T(L"Spielen", L"Play"), IDI_ERROR_);
        delete t;
        return 0;
    }
    case WM_GAME_CRASHED: {
        wchar_t c[16];
        swprintf(c, 16, L"0x%08X", (unsigned)wp);
        if (msg(T(L"Das Spiel wurde unerwartet beendet (Code ", L"The game closed unexpectedly (code ") + std::wstring(c) +
                    T(L").\n\nJetzt einen Fehlerbericht erstellen?", L").\n\nCreate a bug report now?"),
                T(L"Absturz", L"Crash"), IDI_ERROR_, MB_YESNO) == IDYES)
            report_bug();
        return 0;
    }
    case WM_TIMER:   // alle 6 h Update-Check
        if (wp == 1) update_check_async();
        return 0;
    case WM_UPDATE_FOUND:
        if (g_busy && !g_update_shown) {   // Bau läuft: später
            delete g_update_pending;
            g_update_pending = (UpdateInfo*)lp;
        } else if (!g_update_shown) show_update_box((UpdateInfo*)lp);
        else delete (UpdateInfo*)lp;
        return 0;
    case WM_UPDATE_DONE: {
        std::unique_ptr<std::wstring> err((std::wstring*)lp);
        if (err->empty()) {   // Skript starten, dann beenden
            // ".\" davor: NoDefaultCurrentDirectoryInExePath
            std::wstring cl = (known_folder(FOLDERID_System) / L"cmd.exe").wstring() + L" /d /c .\\update.cmd";
            STARTUPINFOW si = {sizeof si};
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, update_dir().c_str(), &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                DestroyWindow(w);
                return 0;
            }
            *err = T(L"Das Update-Skript konnte nicht gestartet werden.", L"The update script could not be started.");
        }
        g_busy = false;
        g_update_shown = false;
        ShowWindow(g_progress, SW_HIDE);
        refresh_tiles();
        SetWindowTextW(g_status, T(L"Update fehlgeschlagen.", L"Update failed."));
        msg(T(L"Das Update konnte nicht installiert werden:\n", L"The update could not be installed:\n") + *err +
                T(L"\n\nPrüfe deine Internetverbindung. Beim nächsten Start des Launchers wird es erneut versucht.",
                  L"\n\nCheck your internet connection. It will be tried again the next time the launcher starts."),
            T(L"Update", L"Update"), IDI_ERROR_);
        return 0;
    }
    case WM_BUILD_DONE: {
        auto* err = (std::wstring*)lp;
        g_busy = false;
        ShowWindow(g_progress, SW_HIDE);
        refresh_tiles();
        if (err->empty()) {
            g_state[std::wstring(L"built_") + GAMES[wp].key] = VERSION;   // für Neubau nach Update
            save_state();
            SetWindowTextW(g_status, (std::wstring(GAMES[wp].title) + T(L" ist installiert. Prüfe kurz die Einstellungen, dann \"Spielen\".",
                                                                      L" is installed. Check the settings, then \"Play\".")).c_str());
            write_default_settings();
            msg(std::wstring(GAMES[wp].title) + T(L" ist installiert.\nPrüfe kurz die Einstellungen, dann \"Spielen\".",
                                                  L" is installed.\nCheck the settings, then \"Play\"."), L"Installation", IDI_SUCCESS);
            show_page(true);
        } else {
            SetWindowTextW(g_status, T(L"Installation fehlgeschlagen.", L"Installation failed."));
            msg(*err, L"Installation", IDI_ERROR_);
            if (!game_ready((int)wp)) remove_dir(game_dir((int)wp));   // keine Reste (ISO-Kopie, EBOOT) eines abgebrochenen Baus
        }
        const bool ok = err->empty();
        delete err;
        if (g_update_pending) {   // Update von vorhin zeigen
            UpdateInfo* u = g_update_pending;
            g_update_pending = nullptr;
            show_update_box(u);
        } else if (ok) rebuild_outdated();   // nur nach Erfolg, sonst Endlosschleife
        return 0;
    }
    case WM_CLOSE:   // beim Schließen speichern
        if (g_settings_page) save_settings_ui();
        if (IsWindowVisible(g_test_status)) save_test();
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, um, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdip = 0;
    Gdiplus::GdiplusStartup(&gdip, &gsi, nullptr);
    const int ids[2][5] = {{IDP_GAME_1ST, IDP_GAME_2ND, IDP_GAME_EXTEND, IDP_LOADING, IDP_WARNING},
                           {IDP_GAME_1ST_W, IDP_GAME_2ND_W, IDP_GAME_EXTEND_W, IDP_LOADING_W, IDP_WARNING_W}};
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 5; i++) g_img_dark[k][i] = load_png(ids[k][i]);
    find_layout();
    g_state = read_kv(L.cfg / L"launcher.ini");
    {   // Sprache: gespeichert, sonst Windows (de -> de, sonst en)
        const std::wstring lang = g_state[L"lang"];
        g_en = lang == L"en" || (lang != L"de" && PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_GERMAN);
    }
    bool updated = false;
    {   // Aufrufe ohne Fenster
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        // --install-test <iso>: baut 1st, Ergebnis in work\install-test.txt
        if (argc == 3 && wcscmp(argv[1], L"--install-test") == 0) {
            fs::create_directories(L.data / L"work");
            const std::wstring err = build_game(0, argv[2]);
            std::ofstream f(L.data / L"work" / L"install-test.txt");
            f << (err.empty() ? "OK" : u8(err)) << "\n";
            if (err.empty()) {
                g_state[std::wstring(L"built_") + GAMES[0].key] = VERSION;
                g_state[std::wstring(L"iso_") + GAMES[0].key] = own_iso(0).wstring();
                save_state();
            }
            return err.empty() ? 0 : 1;
        }
        if (argc == 2 && wcscmp(argv[1], L"--uninstall") == 0) {   // aus "Apps"
            uninstall_all();
            CoUninitialize();
            return 0;
        }
        updated = argc == 2 && wcscmp(argv[1], L"--updated") == 0;
        LocalFree(argv);
    }
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"DivaLauncher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);
    RECT rc = {0, 0, 760, 470};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    g_wnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"DIVA PSP Recomp", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    build_ui();
    apply_theme();
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    if (updated) {   // nach update.cmd
        std::error_code ec;
        fs::remove_all(update_dir(), ec);
        const std::wstring target = g_state[L"update_target"];
        g_state.erase(L"update_target");
        save_state();
        if (target.empty() || cmp_ver(parse_ver(u8(target)), parse_ver(u8(VERSION))) == 0)
            msg(T(L"Update installiert: Version ", L"Update installed: version ") + std::wstring(VERSION), L"DIVA PSP Recomp", IDI_SUCCESS);
        else
            msg(T(L"Das Update auf ", L"The update to ") + target +
                    T(L" hat nicht geklappt (Launcher-Datei gesperrt?). Details: work\\update-log.txt. Es wird erneut versucht.",
                      L" did not work (launcher file locked?). Details: work\\update-log.txt. It will be tried again."),
                L"DIVA PSP Recomp", IDI_ERROR_);
    }
    if (g_state[L"privacy_seen"] != L"1") {   // einmalig: Hinweis Netz
        msg(T(L"Kurz zum Datenschutz:\n\n"
              L"- Beim Start fragt das Programm die Uhrzeit bei pool.ntp.org ab (für das Ende der Alpha).\n"
              L"- Der Launcher sucht auf GitHub nach Updates.\n"
              L"Dabei sehen diese Server deine IP-Adresse, sonst wird nichts übertragen.\n\n"
              L"\"Fehler melden\" legt eine ZIP mit Logs, Einstellungen und PC-Infos (Windows, CPU, Grafikkarte) auf deinen "
              L"Desktop. Sie wird nicht automatisch verschickt, nur wenn du sie selbst schickst.",
              L"A quick note on privacy:\n\n"
              L"- At startup the program asks pool.ntp.org for the time (for the end of the alpha).\n"
              L"- The launcher checks GitHub for updates.\n"
              L"These servers see your IP address, nothing else is sent.\n\n"
              L"\"Report a bug\" puts a ZIP with logs, settings and PC info (Windows, CPU, graphics card) on your desktop. "
              L"It is never sent automatically, only if you send it yourself."),
            T(L"Datenschutz", L"Privacy"), IDI_ABOUT);
        g_state[L"privacy_seen"] = L"1";
        save_state();
    }
    if (alpha::check(alpha_test_arg()).state == alpha::State::Expired) alpha_expired_box();   // T90 Fall A
    rebuild_outdated();                                // veraltete Spiele
    update_check_async();                              // T87
    SetTimer(g_wnd, 1, 6u * 3600u * 1000u, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
