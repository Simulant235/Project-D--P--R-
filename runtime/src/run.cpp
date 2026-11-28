// Einstieg der Spiel-.exe
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <windows.h>

#include "alpha_guard.h"
#include "rt.h"

namespace rt {

Options g_opt;

// T41: settings.ini des Launchers
static void load_settings(const std::string& path) {
    std::ifstream f(path);
    auto trim = [](std::string s) {
        const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        const bool on = v == "1" || v == "on" || v == "true";
        if (k == "save_dir") g_opt.save_dir = v;
        else if (k == "window") g_opt.window = on;
        else if (k == "overlay") g_opt.overlay = on;
        else if (k == "mute") g_opt.mute = on;
        else if (k == "lang") g_opt.lang = v;
    }
}

// Absturz: Log noch schreiben
static LONG WINAPI on_crash(EXCEPTION_POINTERS* e) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)e->ExceptionRecord->ExceptionAddress, &mod);
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    psp_log(PSP_LOG_ERROR, "CRASH", "Ausnahme 0x%08X an 0x%p (%s+0x%llX), Thread %lu", (unsigned)e->ExceptionRecord->ExceptionCode,
            e->ExceptionRecord->ExceptionAddress, base ? base + 1 : name,
            (unsigned long long)((char*)e->ExceptionRecord->ExceptionAddress - (char*)mod), GetCurrentThreadId());
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2)
        psp_log(PSP_LOG_ERROR, "CRASH", "%s an Adresse 0x%llX", e->ExceptionRecord->ExceptionInformation[0] ? "Schreibzugriff" : "Lesezugriff",
                (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
    log_shutdown();
    return EXCEPTION_CONTINUE_SEARCH;
}

static void usage() {
    fprintf(stderr,
            "Aufruf: <spiel>.exe --iso <pfad> [--eboot <pfad>] [--log <datei>] [--log-level E|W|I|D|V]\n"
            "                    [--scale 1..6] [--save-dir <ordner>] [--window] [--ipc <id>] [--dump-threads <sekunden>] [--watch <adresse>] [--shot <sekunden>] [--press <s>:<taste>] [--mute] [--input-log] [--overlay]\n");
}

int run(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--settings") == 0) g_opt.settings_path = argv[i + 1];
    if (!g_opt.settings_path.empty()) load_settings(g_opt.settings_path);
    for (int i = 1; i < argc; i++) {
        auto value = [&](const char* name, std::string& dst) {
            if (strcmp(argv[i], name) != 0) return false;
            if (i + 1 >= argc) { usage(); exit(EXIT_USAGE); }
            dst = argv[++i];
            return true;
        };
        std::string level, scale, dump, watch;
        if (value("--iso", g_opt.iso) || value("--eboot", g_opt.eboot) || value("--log", g_opt.log_path) ||
            value("--save-dir", g_opt.save_dir) || value("--ipc", g_opt.ipc) || value("--settings", g_opt.settings_path) ||
            value("--alpha-test", g_opt.alpha_test))
            continue;
        if (value("--log-level", level)) {
            static const char letters[] = "EWIDV";
            const char* p = level.size() == 1 ? strchr(letters, level[0]) : nullptr;
            if (!p) { usage(); return EXIT_USAGE; }
            g_opt.log_level = (PspLogLevel)(p - letters);
            continue;
        }
        if (value("--scale", scale)) {
            g_opt.scale = atoi(scale.c_str());
            if (g_opt.scale < 1 || g_opt.scale > 6) { usage(); return EXIT_USAGE; }
            continue;
        }
        if (value("--press", dump)) {
            static const struct { const char* n; uint32_t bit; } names[] = {
                {"select", 0x1}, {"start", 0x8}, {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80}, {"l", 0x100},
                {"r", 0x200}, {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000}};
            const size_t colon = dump.find(':');
            uint32_t bit = 0;
            for (const auto& n : names)
                if (colon != std::string::npos && dump.substr(colon + 1) == n.n) bit = n.bit;
            if (!bit) { usage(); return EXIT_USAGE; }
            g_opt.presses.push_back({(uint64_t)(atof(dump.c_str()) * 1000000.0), bit});
            continue;
        }
        if (value("--shot", dump)) { g_opt.shots.push_back(atoi(dump.c_str())); continue; }
        if (value("--watch", watch)) { g_opt.watch.push_back((uint32_t)strtoul(watch.c_str(), nullptr, 0)); continue; }
        if (value("--dump-threads", dump)) { g_opt.dump_threads_sec = atoi(dump.c_str()); continue; }
        if (strcmp(argv[i], "--window") == 0) { g_opt.window = true; continue; }
        if (strcmp(argv[i], "--mute") == 0) { g_opt.mute = true; continue; }
        if (strcmp(argv[i], "--input-log") == 0) { g_opt.input_log = true; continue; }
        if (strcmp(argv[i], "--overlay") == 0) { g_opt.overlay = true; continue; }
        fprintf(stderr, "unbekannter Parameter: %s\n", argv[i]);
        usage();
        return EXIT_USAGE;
    }
    if (g_opt.iso.empty()) {
        usage();
        return EXIT_USAGE;
    }

    log_init(g_opt.log_path, g_opt.log_level);
    SetUnhandledExceptionFilter(on_crash);
    {   // T90: Fall A beendet sauber, Fall B greift auf dem Titelbildschirm (window.cpp)
        const alpha::Result ar = alpha::check(g_opt.alpha_test.c_str());
        if (ar.state == alpha::State::Expired) {
            psp_log(PSP_LOG_INFO, "BOOT", "Alpha-Testphase beendet");
            MessageBoxW(nullptr,
                        g_opt.lang == "en"
                            ? L"The alpha test phase for this version has ended.\n\nThanks for testing! The first official release will be published soon."
                            : L"Die Alpha-Testphase für diese Version ist beendet.\n\nDanke fürs Testen! Bald wird der erste offizielle Release veröffentlicht.",
                        L"DIVA PSP Recomp", MB_ICONINFORMATION | MB_OK);
            log_shutdown();
            return EXIT_OK;
        }
        g_opt.alpha_tamper = ar.state == alpha::State::Tampered;
    }
    psp_log(PSP_LOG_INFO, "BOOT", "diva-psp-recomp runtime, iso=%s", g_opt.iso.c_str());
    std::string error;
    if (!g_iso.open(g_opt.iso, &error)) fatal(EXIT_NOT_FOUND, "BOOT", "%s", error.c_str());
    mem_init();
    window_init();
    load_game();
    scheduler_run();
    psp_log(PSP_LOG_INFO, "BOOT", "exit code %d", exit_code());
    log_shutdown();
    fflush(nullptr);
    _exit(exit_code());
}

} // namespace rt
