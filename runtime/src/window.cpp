// --window: zeigt pro VBlank den Framebuffer aus sceDisplaySetFrameBuf (480x272, 565/5551/4444/8888 wie PPSSPP).
// Bis der GE-Renderer da ist, steht nur der VRAM-Inhalt drin.
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <cstring>
#include <vector>

#include "rt.h"
#include "../hle/audio_out.h"

namespace rt {

void overlay_draw(HDC dc);   // overlay.cpp

namespace {
HWND g_wnd = nullptr;

// --input-log: je Aktionstaste Zeit bis das Spiel sie liest, bis zum nächsten Bild und bis zum Treffer-Ton (+ Tonpuffer).
// Echte Uhr, Tastatur mit 1 kHz, Controller erst beim Spiel-Lesen.
uint64_t wall_us() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (uint64_t)(n.QuadPart * 1000000 / f.QuadPart);
}
struct InRec { uint32_t bit = 0; uint64_t down = 0, game = 0, shown = 0; bool open = false; };
InRec g_in;
std::atomic<uint64_t> g_key_down[4];          // Zeit des letzten Tastendrucks W, D, S, A
const uint32_t FACE_BITS[4] = {0x1000, 0x2000, 0x4000, 0x8000};
const char* const FACE_NAMES[4] = {"Dreieck", "Kreis", "Kreuz", "Quadrat"};

void in_close(const char* why) {
    if (!g_in.open) return;
    g_in.open = false;
    int i = 0; while (i < 3 && FACE_BITS[i] != g_in.bit) i++;
    if (g_in.shown) overlay_latency((g_in.shown - g_in.down) / 1000.0);
    if (!g_opt.input_log) return;
    psp_log(PSP_LOG_INFO, "INPUT", "%s: Taste->Spiel %.1f ms, ->Bild %.1f ms, Tonpuffer %d ms, %s", FACE_NAMES[i],
            (g_in.game - g_in.down) / 1000.0, g_in.shown ? (g_in.shown - g_in.down) / 1000.0 : -1.0, audio_queued_ms(), why);
}

void key_poller() {
    name_thread(L"Eingabe");
    const int vk[4] = {'W', 'D', 'S', 'A'}, vk2[4] = {'I', 'L', 'K', 'J'};
    bool prev[4] = {};
    timeBeginPeriod(1);
    for (;;) {
        for (int i = 0; i < 4; i++) {
            const bool d = ((GetAsyncKeyState(vk[i]) | GetAsyncKeyState(vk2[i])) & 0x8000) != 0;
            if (d && !prev[i]) g_key_down[i] = wall_us();
            prev[i] = d;
        }
        Sleep(1);
    }
}
uint32_t g_fb_addr = 0, g_fb_stride = 512, g_fb_format = 3;
std::vector<uint32_t> g_pixels(480 * 272);

// Vollbild: randlos über den Monitor
void toggle_fullscreen(HWND w) {
    static bool full = false;
    static RECT saved;
    static LONG saved_style;
    if (!full) {
        saved_style = GetWindowLongW(w, GWL_STYLE);
        GetWindowRect(w, &saved);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(w, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(w, GWL_STYLE, saved_style);
        SetWindowPos(w, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER);
    }
    full = !full;
}

LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if ((msg == WM_KEYDOWN && wp == VK_F11) || (msg == WM_SYSKEYDOWN && wp == VK_RETURN)) {   // Vollbild an/aus
        toggle_fullscreen(w);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_F3) { overlay_toggle(); return 0; }   // Leistungsanzeige
    if (msg == WM_KEYDOWN && wp == VK_F4) { overlay_save(); return 0; }     // letzte 15 s der Leistungsanzeige speichern
    if (msg == WM_ERASEBKGND) return 1;   // Ränder malt window_vblank
    if (msg == WM_CLOSE) {
        request_exit(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

// PSP-Pixel (R im niedrigsten Teil) nach 0x00RRGGBB für ein DIB
uint32_t to_bgra(uint32_t c, uint32_t fmt) {
    uint32_t r, g, b;
    switch (fmt) {
    case 0: r = (c & 31) << 3; g = ((c >> 5) & 63) << 2; b = ((c >> 11) & 31) << 3; break;   // 565
    case 1: r = (c & 31) << 3; g = ((c >> 5) & 31) << 3; b = ((c >> 10) & 31) << 3; break;   // 5551
    case 2: r = (c & 15) << 4; g = ((c >> 4) & 15) << 4; b = ((c >> 8) & 15) << 4; break;    // 4444
    default: r = c & 255; g = (c >> 8) & 255; b = (c >> 16) & 255; break;                    // 8888
    }
    return (r << 16) | (g << 8) | b;
}
} // namespace

void display_set_framebuf(uint32_t addr, uint32_t stride, uint32_t format) {
    if (addr && addr != g_fb_addr) overlay_frame();   // Bildwechsel
    g_fb_addr = addr;
    if (stride) g_fb_stride = stride;
    g_fb_format = format & 3;
}

void window_init() {
    name_thread(L"Spiel");   // Hauptthread: Scheduler mit allen PSP-Threads
    if (!g_opt.window) return;
    std::thread(key_poller).detach();   // Tastenzeitpunkte für Latenz (Overlay, --input-log)
    if (g_opt.overlay) overlay_toggle();
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.lpszClassName = L"DivaPspRecomp";
    RegisterClassW(&wc);
    RECT rc = {0, 0, 480 * g_opt.scale, 272 * g_opt.scale};
    const DWORD style = WS_OVERLAPPEDWINDOW;   // frei skalierbar, Bild behält 480:272
    AdjustWindowRect(&rc, style, FALSE);
    g_wnd = CreateWindowW(wc.lpszClassName, L"DIVA PSP Recomp", style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                          rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_wnd) psp_log(PSP_LOG_WARN, "WINDOW", "CreateWindow fehlgeschlagen (%lu)", GetLastError());
}

// --shot <sekunden>: Bild des Framebuffers als generated/shot_<sekunden>.bmp speichern (zum Prüfen ohne Fenster)
static void save_shot(int sec) {
    char name[64];
    snprintf(name, sizeof name, "generated/shot_%03d.bmp", sec);
    FILE* f = fopen(name, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof ih; ih.biWidth = 480; ih.biHeight = -272; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + 480 * 272 * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(g_pixels.data(), 4, 480 * 272, f);
    fclose(f);
    psp_log(PSP_LOG_INFO, "WINDOW", "Bild gespeichert: %s (fb=0x%08X fmt=%u stride=%u)", name, g_fb_addr, g_fb_format, g_fb_stride);
}

// Fall B: Titelbild am türkisen Kreis unten links erkennen, nach 0,7 s einfrieren + Fehler
static void alpha_tamper_tick() {
    static uint64_t seen = 0;
    if (!seen) {
        int teal = 0, n = 0;
        for (int y = 200; y < 272; y += 2)
            for (int x = 0; x < 120; x += 2, n++) {
                const uint32_t c = g_pixels[(size_t)y * 480 + (size_t)x];
                const int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                if (b > 150 && g > 120 && r < 120) teal++;
            }
        if (teal * 100 > n * 20) seen = wall_us();
        return;
    }
    if (wall_us() - seen < 700000) return;
    // Hallo, du hast es gefunden! Dieser „Grafikfehler“ ist keiner: Er ist die Absturz-Imitation der Alpha (Fall B) und
    // kommt nur, wenn die Windows-Uhr verstellt ist. Mit richtiger Uhr (Windows: „Uhrzeit automatisch festlegen“) startet
    // das Spiel wieder ganz normal.
    // Und weil du dir die Mühe gemacht hast, so tief in den Code zu schauen, hast du es dir verdient, auch nach der Alpha
    // weiterzuspielen. So geht's (im Installationsordner %LOCALAPPDATA%\DIVA PSP Recomp):
    //   1. In src\cmake\alpha.cmake die Zeile  set(DIVA_ALPHA_EXPIRES "…")  in  set(DIVA_ALPHA_EXPIRES "")  ändern
    //      (leer = kein Ablaufdatum).
    //   2. Den Ordner games\1st löschen und den Launcher starten: Er baut das Spiel ohne Ablaufdatum neu.
    //   3. Der Knopf „Spielen“ im Launcher prüft das Datum selbst noch. Darum das Spiel direkt starten:
    //      games\1st\diva1st.exe --iso games\1st\diva1st.iso --settings config\settings.ini --save-dir saves
    //      (aus dem Installationsordner heraus, z.B. als Verknüpfung).
    // Ein Update überschreibt src\ wieder; dann Schritt 1 und 2 wiederholen. Viel Spaß, und danke fürs Testen!
    audio_freeze();
    psp_log(PSP_LOG_ERROR, "WINDOW", "FATAL: Failed to create D3D11 rendering context [0x887A0020]");
    Sleep(900);
    MessageBoxW(g_wnd, L"FATAL: Failed to create D3D11 rendering context [0x887A0020]. Check GPU drivers.", L"DIVA PSP Recomp",
                MB_ICONERROR | MB_OK);
    exit_process(EXIT_DEVICE);
}

// pro VBlank aus dem Scheduler: Nachrichten, Framebuffer zeigen
void window_vblank(const uint8_t* mem) {
    if (g_in.open && !g_in.shown) g_in.shown = wall_us();
    if (g_in.open && wall_us() > g_in.game + 400000) in_close("Treffer: noch nicht erkennbar");
    static size_t next_shot = 0;
    const bool shot = next_shot < g_opt.shots.size() && time_us() >= (uint64_t)g_opt.shots[next_shot] * 1000000ull;
    if (!g_wnd && !shot) return;
    MSG m;
    while (g_wnd && PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (!g_fb_addr) return;
    const uint32_t base = g_fb_addr & 0x0FFFFFFF;   // ohne Cache-/Spiegelbits
    const uint32_t bpp = g_fb_format == 3 ? 4 : 2;
    if (!mem_valid(base, g_fb_stride * 271 * bpp + 480 * bpp)) return;
    for (uint32_t y = 0; y < 272; y++) {
        const uint8_t* row = mem + base + y * g_fb_stride * bpp;
        uint32_t* out = &g_pixels[y * 480];
        for (uint32_t x = 0; x < 480; x++) {
            uint32_t c;
            if (bpp == 4) memcpy(&c, row + x * 4, 4);
            else c = (uint32_t)row[x * 2] | ((uint32_t)row[x * 2 + 1] << 8);
            out[x] = to_bgra(c, g_fb_format);
        }
    }
    if (shot) save_shot(g_opt.shots[next_shot++]);
    if (g_opt.alpha_tamper) alpha_tamper_tick();
    if (!g_wnd) return;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 480;
    bi.bmiHeader.biHeight = -272;   // von oben nach unten
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    // Ins Fenster einpassen (Seitenverhältnis 480:272, schwarze Ränder)
    RECT cr;
    GetClientRect(g_wnd, &cr);
    const int cw = cr.right, ch = cr.bottom;
    int dw = cw, dh = cw * 272 / 480;
    if (dh > ch) { dh = ch; dw = ch * 480 / 272; }
    const int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
    HDC dc = GetDC(g_wnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, 480, 272, g_pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT bars[4] = {{0, 0, cw, dy}, {0, dy + dh, cw, ch}, {0, dy, dx, dy + dh}, {dx + dw, dy, cw, dy + dh}};
    for (const RECT& r : bars)
        if (r.right > r.left && r.bottom > r.top) FillRect(dc, &r, black);
    overlay_draw(dc);
    ReleaseDC(g_wnd, dc);
}

// sceCtrl-Tasten: Controller, Tastatur (nur im Vordergrund), --press <s>:<taste> (0,25 s ab Sekunde s).
// Pfeile = Steuerkreuz, Enter = START, Leertaste = SELECT,
// W/I = Dreieck, D/L = Kreis, S/K = Kreuz, A/J = Quadrat, Q = L, E = R.
uint32_t ctrl_buttons() {
    uint32_t b = 0;
    if (g_wnd && GetForegroundWindow() == g_wnd) {
        static const struct { int vk; uint32_t bit; } map[] = {
            {VK_RETURN, 0x8}, {VK_SPACE, 0x1}, {VK_UP, 0x10}, {VK_RIGHT, 0x20}, {VK_DOWN, 0x40}, {VK_LEFT, 0x80},
            {'Q', 0x100}, {'E', 0x200}, {'W', 0x1000}, {'D', 0x2000}, {'S', 0x4000}, {'A', 0x8000},
            {'I', 0x1000}, {'L', 0x2000}, {'K', 0x4000}, {'J', 0x8000}};
        for (const auto& m : map)
            if (GetAsyncKeyState(m.vk) & 0x8000) b |= m.bit;
    }
#ifdef DIVA_HAVE_SDL3
    b |= pad_sdl_buttons();   // alle Controller über SDL3 (pad_sdl.cpp)
#else
    b |= pad_ps_buttons();   // PlayStation-Controller (pad_ps.cpp)
    // Xbox-Controller (XInput, erster Controller), Belegung nach Position: A (unten) = Kreuz, B (rechts) = Kreis,
    // X (links) = Quadrat, Y (oben) = Dreieck, Start = START, Back = SELECT, LB/RB = L/R, Steuerkreuz und linker Stick
    XINPUT_STATE xs = {};
    if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
        static const struct { WORD x; uint32_t bit; } pad[] = {
            {XINPUT_GAMEPAD_A, 0x4000}, {XINPUT_GAMEPAD_B, 0x2000}, {XINPUT_GAMEPAD_X, 0x8000}, {XINPUT_GAMEPAD_Y, 0x1000},
            {XINPUT_GAMEPAD_START, 0x8}, {XINPUT_GAMEPAD_BACK, 0x1}, {XINPUT_GAMEPAD_DPAD_UP, 0x10},
            {XINPUT_GAMEPAD_DPAD_RIGHT, 0x20}, {XINPUT_GAMEPAD_DPAD_DOWN, 0x40}, {XINPUT_GAMEPAD_DPAD_LEFT, 0x80},
            {XINPUT_GAMEPAD_LEFT_SHOULDER, 0x100}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, 0x200}};
        for (const auto& m : pad)
            if (xs.Gamepad.wButtons & m.x) b |= m.bit;
        if (xs.Gamepad.sThumbLY > 16000) b |= 0x10;
        if (xs.Gamepad.sThumbLY < -16000) b |= 0x40;
        if (xs.Gamepad.sThumbLX > 16000) b |= 0x20;
        if (xs.Gamepad.sThumbLX < -16000) b |= 0x80;
    }
#endif
    const uint64_t now = time_us();
    for (const auto& p : g_opt.presses)
        if (now >= p.first && now < p.first + 250000) b |= p.second;
    if (g_wnd) {   // neu gedrückte Aktionstaste: Latenzmessung beginnen (Overlay, --input-log)
        static uint32_t last = 0;
        static uint64_t rel_seen[4] = {};   // letzte Abfrage, bei der das Spiel die Taste losgelassen sah
        const uint32_t fresh = b & ~last;
        last = b;
        const uint64_t t = wall_us();
        bool started = false;
        for (int i = 0; i < 4; i++) {
            if (!(b & FACE_BITS[i])) { rel_seen[i] = t; continue; }
            if (started || !(fresh & FACE_BITS[i])) continue;
            in_close("Treffer: noch nicht erkennbar");
            // nur wenn nach dem letzten gesehenen Loslassen (sonst z.B. Doppeltippen zwischen zwei Abfragen, Messung zu lang)
            const uint64_t k = g_key_down[i];
            g_in = InRec{FACE_BITS[i], (k && k >= rel_seen[i] && t - k < 200000) ? k : t, t, 0, true};
            started = true;
        }
    }
    return b;
}

void input_log_sound(int slot, int peak) {
    if (!g_in.open || slot == 6 || peak < 2000) return;   // Slot 6 = Musik
    const uint64_t t = wall_us();
    char why[96];
    snprintf(why, sizeof why, "->Effekt-Ton %.1f ms (+ Tonpuffer %d ms) Slot %d", (t - g_in.down) / 1000.0, audio_queued_ms(), slot);
    in_close(why);
}

} // namespace rt
