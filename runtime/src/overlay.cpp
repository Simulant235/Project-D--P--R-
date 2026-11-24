// Leistungsanzeige (F3, --overlay). FPS, Bildzeit, Latenz Taste->Bild, Tonpuffer, CPU/RAM/GPU.
// CPU/GPU per PDH, 1x pro Sekunde in eigenem Thread.
#include <windows.h>
#include <shellapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <map>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rt.h"
#include "../hle/audio_out.h"

namespace rt {

namespace {
std::atomic<bool> g_on{false};
std::mutex g_mx;
std::string g_sys_text;
const char* tr(const char* de, const char* en) { return g_opt.lang == "en" ? en : de; }   // Sprache des Launchers

uint64_t now_us() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (uint64_t)(n.QuadPart * 1000000 / f.QuadPart);
}

// FPS: Bildwechsel pro Sekunde (echte Uhr)
int g_fps_1low = 0;                 // 1% low: Mittel der langsamsten 1 % (letzte 1200 Bilder)
std::vector<uint32_t> g_ft_ring;     // Bildzeiten in µs
size_t g_ft_pos = 0;
uint64_t g_sec_start = 0, g_last_flip = 0, g_worst_ft = 0, g_worst_ft_shown = 0;
int g_frames = 0, g_fps = 0, g_fps_min = 0;
int g_min_secs = 0;
int g_warmup = 5;   // erste 5 s nicht für den Tiefstwert

// Latenz Taste->Bild
double g_lat_last = 0, g_lat_max = 0, g_lat_sum = 0;
int g_lat_n = 0;

void sampler() {
    name_thread(L"Overlay");
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER cpu = nullptr, gpu = nullptr;
    const bool pdh_ok = PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS &&
                        PdhAddEnglishCounterW(q, L"\\Processor(*)\\% Processor Time", 0, &cpu) == ERROR_SUCCESS;
    const bool gpu_ok = pdh_ok && PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu) == ERROR_SUCCESS;
    if (pdh_ok) PdhCollectQueryData(q);
    SYSTEM_INFO si; GetSystemInfo(&si);
    const int ncpu = (int)si.dwNumberOfProcessors;
    FILETIME c0, e0, k0, u0; GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    uint64_t t0 = now_us();
    auto ft = [](const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime) / 10; };
    uint64_t proc0 = ft(k0) + ft(u0);
    // Logischer Prozessor (Thread) -> physischer Kern
    std::vector<int> core_of(64, -1);
    int ncores = 0;
    {
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        std::vector<uint8_t> b(len);
        if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)b.data(), &len)) {
            for (DWORD off = 0; off < len;) {
                auto* e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(b.data() + off);
                const KAFFINITY m = e->Processor.GroupMask[0].Mask;
                for (int i = 0; i < 64; i++)
                    if (m & ((KAFFINITY)1 << i)) core_of[(size_t)i] = ncores;
                ncores++;
                off += e->Size;
            }
        }
    }
    wchar_t pid_tag[32];
    swprintf(pid_tag, 32, L"pid_%lu_", GetCurrentProcessId());
    std::map<DWORD, uint64_t> thr_prev;   // Thread-ID -> CPU-Zeit (µs) bei der letzten Messung
    std::vector<uint8_t> buf;
    for (;;) {
        Sleep(1000);   // 1x pro Sekunde, GPU-Abfrage kostet selbst Zeit
        std::string s;
        char line[256];
        // Programm-CPU (Anteil an der ganzen CPU und in "Kernen")
        FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        const uint64_t t = now_us(), proc = ft(k) + ft(u);
        const double used = (double)(proc - proc0) / (double)(t - t0);   // 1.0 = ein Kern voll
        const uint64_t dt = std::max<uint64_t>(t - t0, 1);
        proc0 = proc; t0 = t;
        double total = -1;
        std::vector<int> cores;
        double gpu3d = -1;
        if (pdh_ok && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            DWORD size = 0, n = 0;
            if (PdhGetFormattedCounterArrayW(cpu, PDH_FMT_DOUBLE, &size, &n, nullptr) == PDH_MORE_DATA) {
                buf.resize(size);
                auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
                if (PdhGetFormattedCounterArrayW(cpu, PDH_FMT_DOUBLE, &size, &n, it) == ERROR_SUCCESS) {
                    std::vector<std::pair<int, int>> v;
                    for (DWORD i = 0; i < n; i++) {
                        if (wcscmp(it[i].szName, L"_Total") == 0) total = it[i].FmtValue.doubleValue;
                        else v.push_back({_wtoi(it[i].szName), (int)(it[i].FmtValue.doubleValue + 0.5)});
                    }
                    std::sort(v.begin(), v.end());
                    for (auto& p : v) cores.push_back(p.second);
                }
            }
            size = 0; n = 0;
            if (gpu_ok && PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &size, &n, nullptr) == PDH_MORE_DATA) {
                buf.resize(size);
                auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
                if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &size, &n, it) == ERROR_SUCCESS) {
                    gpu3d = 0;
                    for (DWORD i = 0; i < n; i++)
                        if (wcsstr(it[i].szName, pid_tag) && wcsstr(it[i].szName, L"engtype_3D")) gpu3d += it[i].FmtValue.doubleValue;
                    gpu3d = std::min(gpu3d, 100.0);
                }
            }
        }
        snprintf(line, sizeof line, tr("CPU  Programm %.0f%% (%.1f Threads voll), %d Kerne / %d Threads", "CPU  program %.0f%% (%.1f threads full), %d cores / %d threads"), used * 100.0 / ncpu, used,
                 ncores, ncpu);
        s += line; s += "\n";
        // Last je Kern (nur für den ganzen PC messbar)
        s += std::string(tr("Kern:Threads (ganzer PC, gesamt ", "Core:threads (whole PC, total ")) + (total >= 0 ? std::to_string((int)(total + 0.5)) + "%" : std::string("?")) + ")\n";
        int col = 0;
        for (int kk = 0; kk < ncores; kk++) {
            std::string tl;
            for (size_t i = 0; i < cores.size() && i < 64; i++)
                if (core_of[i] == kk) tl += (tl.empty() ? "" : "/") + std::to_string(cores[i]);
            snprintf(line, sizeof line, tr(" K%d:%-8s", " C%d:%-8s"), kk + 1, tl.c_str());
            s += line;
            if (++col == 4 && kk + 1 < ncores) { s += "\n"; col = 0; }
        }
        s += "\n";
        {   // Last je eigenem Thread, nach Namen gruppiert (100% = ein Thread voll)
            std::map<std::wstring, std::pair<double, int>> grp;
            std::map<DWORD, uint64_t> thr_now;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {sizeof te};
            const DWORD pid = GetCurrentProcessId();
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid) continue;
                    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (!h) continue;
                    FILETIME tc, te2, tk, tu;
                    if (GetThreadTimes(h, &tc, &te2, &tk, &tu)) {
                        const uint64_t cpu_us = ft(tk) + ft(tu);
                        thr_now[te.th32ThreadID] = cpu_us;
                        const auto it = thr_prev.find(te.th32ThreadID);
                        const double pct = it == thr_prev.end() ? 0.0 : (double)(cpu_us - it->second) * 100.0 / (double)dt;
                        PWSTR nm = nullptr;
                        std::wstring name = L"Andere";
                        if (SUCCEEDED(GetThreadDescription(h, &nm)) && nm) {
                            if (*nm) name = nm;
                            LocalFree(nm);
                        }
                        auto& g = grp[name];
                        g.first += pct; g.second++;
                    }
                    CloseHandle(h);
                } while (Thread32Next(snap, &te));
            }
            if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
            thr_prev.swap(thr_now);
            s += tr("Threads (100% = 1 Thread voll)\n", "Threads (100% = 1 thread full)\n");
            int tcol = 0;
            for (const char* want : {"Spiel", "Raster", "Ton", "Eingabe", "Overlay", "Andere"}) {
                std::wstring w(want, want + strlen(want));
                const auto it = grp.find(w);
                if (it == grp.end()) continue;
                static const std::map<std::string, const char*> en = {{"Spiel", "Game"}, {"Ton", "Audio"}, {"Eingabe", "Input"}, {"Andere", "Other"}};
                const char* shown = g_opt.lang == "en" && en.count(want) ? en.at(want) : want;
                if (it->second.second > 1) snprintf(line, sizeof line, " %s %.0f%% (%dx)", shown, it->second.first, it->second.second);
                else snprintf(line, sizeof line, " %s %.0f%%", shown, it->second.first);
                s += line;
                if (++tcol == 3) { s += "\n"; tcol = 0; }
            }
            if (tcol) s += "\n";
        }
        PROCESS_MEMORY_COUNTERS pmc = {sizeof pmc};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        snprintf(line, sizeof line, tr("RAM  Programm %.0f MB", "RAM  program %.0f MB"), pmc.WorkingSetSize / 1048576.0);
        s += line; s += "\n";
        if (gpu3d >= 0) snprintf(line, sizeof line, tr("GPU  Programm 3D %.0f%%", "GPU  program 3D %.0f%%"), gpu3d);
        else snprintf(line, sizeof line, "%s", tr("GPU  nicht messbar", "GPU  not measurable"));
        s += line;
        std::lock_guard<std::mutex> lk(g_mx);
        g_sys_text = s;
    }
}
} // namespace

void name_thread(const wchar_t* name) { SetThreadDescription(GetCurrentThread(), name); }

static void start_sampler() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(sampler).detach(); });
}

void overlay_toggle() {
    g_on = !g_on;
    if (g_on) start_sampler();
}

static std::string overlay_text() {
    std::string s;
    char line[256];
    snprintf(line, sizeof line, tr("FPS  %d (tiefste %d, 1%% low %d), laengstes Bild %.1f ms\n", "FPS  %d (lowest %d, 1%% low %d), longest frame %.1f ms\n"), g_fps, g_fps_min, g_fps_1low, g_worst_ft_shown / 1000.0);
    s += line;
    if (g_lat_n)
        snprintf(line, sizeof line, tr("Latenz Taste->Bild  %.1f ms (O %.1f, hoechste %.1f)\n", "Latency key->frame  %.1f ms (avg %.1f, highest %.1f)\n"), g_lat_last, g_lat_sum / g_lat_n, g_lat_max);
    else
        snprintf(line, sizeof line, "%s", tr("Latenz Taste->Bild  - (W/A/S/D druecken)\n", "Latency key->frame  - (press W/A/S/D)\n"));
    s += line;
    snprintf(line, sizeof line, tr("Tonpuffer  %d ms\n", "Audio buffer  %d ms\n"), audio_queued_ms());
    s += line;
    std::lock_guard<std::mutex> lk(g_mx);
    return s + (g_sys_text.empty() ? std::string(tr("Messung startet ...", "Measuring ...")) : g_sys_text);
}

// F4: je Sekunde ein Stand der Anzeige, die letzten 15 werden gespeichert
static std::mutex g_hist_mx;
static std::deque<std::string> g_hist;

static void history_add() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[64];
    snprintf(head, sizeof head, "--- %02d:%02d:%02d\n", st.wHour, st.wMinute, st.wSecond);
    std::lock_guard<std::mutex> lk(g_hist_mx);
    g_hist.push_back(head + overlay_text());
    while (g_hist.size() > 15) g_hist.pop_front();
}

static std::string history_write(const char* prefix) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[64];
    snprintf(name, sizeof name, "%s-%04d%02d%02d-%02d%02d%02d.txt", prefix, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string dir = g_opt.log_path;
    const size_t slash = dir.find_last_of("\\/");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    const std::string path = dir + name;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return {};
    {
        std::lock_guard<std::mutex> lk(g_hist_mx);
        fprintf(f, tr("DIVA PSP Recomp: Leistungsanzeige (F3), letzte %d Sekunden\n\n", "DIVA PSP Recomp: performance overlay (F3), last %d seconds\n\n"), (int)g_hist.size());
        for (const std::string& h : g_hist) fprintf(f, "%s\n", h.c_str());
    }
    fclose(f);
    return path;
}

void overlay_save() {
    const std::string path = history_write("f3");
    if (path.empty()) return;
    psp_log(PSP_LOG_INFO, "OVERLAY", "F4: %s gespeichert", path.c_str());
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Wächter: Log jede Sekunde flushen, meldet 10 s ohne neues Bild
static void watchdog() {
    name_thread(L"Watchdog");
    bool hung = false;
    for (;;) {
        Sleep(1000);
        const uint64_t last = g_last_flip, t = now_us();
        if (last && t - last > 10000000ull && !hung) {
            hung = true;
            psp_log(PSP_LOG_ERROR, "HANG", "Kein neues Bild seit %llu s, Spiel eingefroren?", (unsigned long long)((t - last) / 1000000));
            const std::string p = history_write("hang");
            if (!p.empty()) psp_log(PSP_LOG_ERROR, "HANG", "Leistungsanzeige der letzten Sekunden: %s", p.c_str());
        } else if (hung && t - last < 2000000ull) {
            hung = false;
            psp_log(PSP_LOG_WARN, "HANG", "Spiel läuft wieder");
        }
        log_flush();
    }
}

// Aus sceDisplaySetFrameBuf, wenn das Spiel ein neues Bild zeigt
void overlay_frame() {
    const uint64_t t = now_us();
    if (g_last_flip) {
        g_worst_ft = std::max(g_worst_ft, t - g_last_flip);
        if (g_warmup <= 0) {
            const uint32_t ft = (uint32_t)std::min<uint64_t>(t - g_last_flip, 10000000ull);
            if (g_ft_ring.size() < 1200) g_ft_ring.push_back(ft);
            else { g_ft_ring[g_ft_pos] = ft; g_ft_pos = (g_ft_pos + 1) % g_ft_ring.size(); }
        }
    }
    g_last_flip = t;
    g_frames++;
    if (!g_sec_start) {
        g_sec_start = t;
        start_sampler();
        std::thread(watchdog).detach();
    }
    if (t - g_sec_start >= 1000000) {
        g_fps = (int)(g_frames * 1000000ull / (t - g_sec_start));
        if (g_warmup > 0) g_warmup--;
        else if (++g_min_secs > 60) { g_fps_min = g_fps; g_min_secs = 1; }   // "tiefste" gilt für die letzten 60 s
        else if (!g_fps_min || g_fps < g_fps_min) g_fps_min = g_fps;
        g_worst_ft_shown = g_worst_ft;
        if (g_ft_ring.size() >= 100) {
            std::vector<uint32_t> v = g_ft_ring;
            const size_t n = v.size() / 100;
            std::nth_element(v.begin(), v.begin() + (n - 1), v.end(), std::greater<uint32_t>());
            uint64_t sum = 0;
            for (size_t i = 0; i < n; i++) sum += v[i];
            g_fps_1low = sum ? (int)(1000000ull * n / sum) : 0;
        }
        g_worst_ft = 0;
        g_frames = 0;
        g_sec_start = t;
        history_add();
    }
}

void overlay_latency(double ms) {
    if (ms <= 0) return;
    g_lat_last = ms;
    g_lat_sum += ms; g_lat_n++;
    g_lat_max = std::max(g_lat_max, ms);
}

void overlay_draw(HDC dc) {
    if (!g_on) return;
    const std::string s = overlay_text();
    static HFONT font = CreateFontW(16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
    HGDIOBJ old = SelectObject(dc, font);
    RECT r = {0, 0, 0, 0};
    DrawTextA(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_LEFT | DT_NOPREFIX);
    r.right += 12; r.bottom += 8;
    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    r.left += 6; r.top += 4;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(80, 255, 80));
    DrawTextA(dc, s.c_str(), -1, &r, DT_LEFT | DT_NOPREFIX);
    SelectObject(dc, old);
}

} // namespace rt
