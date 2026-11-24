// Direktaufrufe des generierten Codes (psp_context.h) und Hooks aus games/*.toml
#include <cstdio>

#include "rt.h"

namespace rt {

std::string guest_format(const uint8_t* mem, uint32_t fmt_addr, const PspContext* ctx, int first_arg) {
    std::string out;
    if (!fmt_addr || !mem_valid(fmt_addr, 1)) return "(null)";
    int arg = first_arg;
    auto next = [&]() -> uint32_t { return arg < 8 ? ctx->r[PSP_A0 + arg++] : 0; };
    for (const char* p = (const char*)mem + fmt_addr; *p && out.size() < 900; p++) {
        if (*p != '%') { out += *p; continue; }
        std::string spec = "%";
        p++;
        while (*p && strchr("-+ #0123456789.", *p)) spec += *p++;
        while (*p == 'l' || *p == 'h') p++;
        char buf[128];
        switch (*p) {
        case 'd': case 'i': snprintf(buf, sizeof buf, (spec + "d").c_str(), (int)next()); out += buf; break;
        case 'u': case 'x': case 'X': case 'c': case 'o': snprintf(buf, sizeof buf, (spec + *p).c_str(), next()); out += buf; break;
        case 'p': snprintf(buf, sizeof buf, "0x%08X", next()); out += buf; break;
        case 's': {
            const uint32_t a = next();
            if (a && mem_valid(a, 1)) out.append((const char*)mem + a, strnlen((const char*)mem + a, 256));
            else out += "(null)";
            break;
        }
        case 'f': case 'g': case 'e': arg = (arg + 1) & ~1; next(); next(); out += "<float>"; break;
        case '%': out += '%'; break;
        case 0: p--; break;
        default: out += spec; out += *p; break;
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

} // namespace rt

extern "C" {

void psp_call_indirect(PspContext* ctx, uint8_t* mem, uint32_t target) {
    if (!(target & 3)) {
        for (int i = 0; g_psp_modules[i]; i++) {
            const PspModuleDesc* m = g_psp_modules[i];
            if (target >= m->code_addr && target - m->code_addr < m->code_size) {
                if (PspFunc f = m->func_table[(target - m->code_addr) >> 2]) {
                    f(ctx, mem);
                    return;
                }
                break;
            }
        }
    }
    rt::fatal(rt::EXIT_TARGET, "RECOMP", "unknown target 0x%08X ra=0x%08X pc=0x%08X", target, ctx->r[PSP_RA], ctx->pc);
}

void psp_unimplemented(PspContext* ctx, uint32_t addr, const char* mnemonic) {
    (void)ctx;
    rt::fatal(rt::EXIT_UNIMPL, "RECOMP", "unimplemented instruction 0x%08X %s", addr, mnemonic);
}

void psp_break(PspContext* ctx, uint32_t addr, uint32_t code) {
    rt::fatal(rt::EXIT_BREAK, "RECOMP", "break 0x%08X code=0x%X ra=0x%08X", addr, code, ctx->r[PSP_RA]);
}

// Hook diva1st.toml (0x08806984): letzter Schritt der C++-Exception-Behandlung, catch-Sprung nicht unterstützt
void psp_rt_cxx_exception_resume(PspContext* ctx, uint8_t* mem) {
    (void)mem;
    rt::fatal(rt::EXIT_CXX, "RECOMP", "c++ exception resume target=0x%08X ra=0x%08X", ctx->r[PSP_A2], ctx->r[PSP_RA]);
}

} // extern "C"
