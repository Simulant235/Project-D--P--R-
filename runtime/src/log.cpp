// Log, Format:  DIVA|<Level>|<Zeit>|<Thread>|<Kategorie>|<Text>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include "rt.h"

namespace rt {

static FILE* g_log = nullptr;
static std::mutex g_close_mx;
static PspLogLevel g_level = PSP_LOG_INFO;

struct UnimplKey {
    std::string lib, name;
    uint32_t nid;
    bool operator<(const UnimplKey& o) const { return lib != o.lib ? lib < o.lib : nid < o.nid; }
};
static std::map<UnimplKey, uint32_t> g_unimpl;
static uint64_t g_last_count_dump = 0;

void log_init(const std::string& path, PspLogLevel level) {
    g_level = level;
    if (!path.empty()) {
        g_log = fopen(path.c_str(), "wb");
        if (!g_log) fprintf(stderr, "Log-Datei kann nicht angelegt werden: %s\n", path.c_str());
    }
}

static void vlog(PspLogLevel level, const char* category, const char* fmt, va_list ap) {
    if (level > g_level) return;
    static const char letters[] = {'E', 'W', 'I', 'D', 'V'};
    char text[1024];
    vsnprintf(text, sizeof text, fmt, ap);
    for (char* p = text; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
    const Thread* t = cur();
    FILE* out = g_log ? g_log : stderr;
    fprintf(out, "DIVA|%c|%010.3f|%s|%s|%s\n", letters[level <= PSP_LOG_VERBOSE ? level : PSP_LOG_VERBOSE],
            (double)time_us() / 1e6, t ? t->name.c_str() : "-", category, text);
    if (level <= PSP_LOG_WARN) fflush(out);
}

static void dump_unimpl_counts() {
    for (const auto& u : g_unimpl)
        psp_log(PSP_LOG_INFO, "HLE", "UNIMPL-COUNT %s 0x%08X %s %u", u.first.lib.c_str(), u.first.nid, u.first.name.c_str(), u.second);
}

void log_flush() {
    std::lock_guard<std::mutex> lk(g_close_mx);
    if (g_log) fflush(g_log);
}

void log_shutdown() {
    dump_unimpl_counts();
    std::lock_guard<std::mutex> lk(g_close_mx);
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
    fflush(stderr);
}

void exit_process(int code) {
    log_shutdown();
    _exit(code);
}

void fatal(int code, const char* category, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(PSP_LOG_ERROR, category, fmt, ap);
    va_end(ap);
    exit_process(code);
}

} // namespace rt

extern "C" {

void psp_log(PspLogLevel level, const char* category, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt::vlog(level, category, fmt, ap);
    va_end(ap);
}

void psp_hle_unimplemented(PspContext* ctx, const char* lib, uint32_t nid, const char* name, uint32_t ret) {
    uint32_t& count = rt::g_unimpl[{lib, name, nid}];
    if (++count <= 3)
        psp_log(PSP_LOG_WARN, "HLE", "UNIMPL %s 0x%08X %s a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X ra=0x%08X", lib, nid, name,
                ctx->r[PSP_A0], ctx->r[PSP_A1], ctx->r[PSP_A2], ctx->r[PSP_A3], ctx->r[PSP_RA]);
    const uint64_t now = rt::time_us();
    if (now - rt::g_last_count_dump > 60000000ull) {
        rt::g_last_count_dump = now;
        rt::dump_unimpl_counts();
    }
    ctx->r[PSP_V0] = ret;
}

} // extern "C"
