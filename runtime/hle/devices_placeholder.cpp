// Platzhalter für sceGe_user, sceDisplay, sceCtrl, sceAudio, sceVaudio, sceRtc (T24), bis Grafik (T21), Ton (T22), Eingabe (T32) dran sind.
//   - Displaylisten nur durchlaufen (FINISH-Callback wird ausgelöst)
//   - Audio wartet nur die Dauer der Samples ab
//   - nie eine Taste gedrückt
#include <algorithm>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "../src/rt.h"
#include "ge_soft.h"
#include "audio_out.h"

using namespace rt;

namespace {

struct GeCallback { uint32_t signal_func = 0, signal_arg = 0, finish_func = 0, finish_arg = 0; bool used = false; };
GeCallback g_ge_cb[16];
int g_ge_list_id = 0x35000000;
uint32_t g_framebuf = 0;

struct AudioChannel { bool reserved = false; uint32_t samples = 0; uint64_t play_until = 0; bool mono = false; };
AudioChannel g_audio[8], g_output2, g_vaudio;
uint32_t g_vaudio_freq = 44100;

// wartet bis der vorige Puffer "abgespielt" wäre, bucht die Dauer des neuen. Rückstand bis 250 ms wird
// ohne Warten aufgeholt (Mittel = 44,1 kHz), darüber neu ansetzen.
void audio_pace(AudioChannel& ch, uint32_t samples, uint32_t freq) {
    const uint64_t dur = (uint64_t)samples * 1000000ull / freq;
    const uint64_t now = time_us();
    if (ch.play_until > now) thread_block(WaitType::Delay, 0, ch.play_until - now);
    else if (ch.play_until + 250000 < now) ch.play_until = now;
    ch.play_until += dur;
}

// Statistik der GE-Befehle (T21)
struct GeStats {
    uint64_t cmd[256] = {};
    uint64_t prim_type[8] = {};
    std::map<uint32_t, uint64_t> vertex_types, tex_formats, tex_sizes, clear_args, fb_addrs, tex_region;
    uint64_t lists = 0, words = 0;
} g_ge_stats;

void log_ge_stats() {
    const GeStats& g = g_ge_stats;
    psp_log(PSP_LOG_INFO, "GE", "STATS lists=%llu words=%llu", (unsigned long long)g.lists, (unsigned long long)g.words);
    std::string top;
    std::vector<std::pair<uint64_t, int>> order;
    for (int i = 0; i < 256; i++)
        if (g.cmd[i]) order.push_back({g.cmd[i], i});
    std::sort(order.rbegin(), order.rend());
    for (const auto& o : order) {
        char b[32];
        snprintf(b, sizeof b, " %02X:%llu", o.second, (unsigned long long)o.first);
        top += b;
    }
    psp_log(PSP_LOG_INFO, "GE", "STATS cmds%s", top.c_str());
    std::string prims;
    for (int i = 0; i < 8; i++) {
        char b[32];
        snprintf(b, sizeof b, " %d:%llu", i, (unsigned long long)g.prim_type[i]);
        prims += b;
    }
    psp_log(PSP_LOG_INFO, "GE", "STATS prim_types%s (0 points,1 lines,2 line strip,3 tris,4 tri strip,5 tri fan,6 sprites)", prims.c_str());
    auto dump = [](const char* name, const std::map<uint32_t, uint64_t>& m) {
        std::string t;
        for (const auto& e : m) {
            char b[40];
            snprintf(b, sizeof b, " %06X:%llu", e.first, (unsigned long long)e.second);
            t += b;
        }
        psp_log(PSP_LOG_INFO, "GE", "STATS %s%s", name, t.c_str());
    };
    dump("vertex_types", g.vertex_types);
    dump("tex_formats", g.tex_formats);
    dump("tex_size_cmds", g.tex_sizes);
    dump("clear_cmd_args", g.clear_args);
    dump("framebuf_ptrs", g.fb_addrs);
    dump("tex_region", g.tex_region);
}

// Displayliste bis END durchlaufen; Argument des letzten FINISH oder -1
int walk_display_list(uint8_t* mem, uint32_t pc, uint32_t stall, uint32_t* end_pc) {
    uint32_t base = 0, offset = 0, stack[32], sp = 0;
    int finish = -1;
    bool finished = false;
    for (int guard = 0; guard < 4000000; guard++) {
        if (stall && pc == stall) break;
        if (!mem_valid(pc, 4)) {
            psp_log(PSP_LOG_ERROR, "GE", "display list leaves memory at 0x%08X", pc);
            break;
        }
        const uint32_t w = psp_r32(mem, pc), cmd = w >> 24, arg = w & 0xFFFFFF;
        pc += 4;
        g_ge_stats.cmd[cmd]++;
        g_ge_stats.words++;
        if (cmd == 0x04) g_ge_stats.prim_type[(arg >> 16) & 7]++;
        else if (cmd == 0x12) g_ge_stats.vertex_types[arg]++;
        else if (cmd == 0xC3) g_ge_stats.tex_formats[arg & 0xF]++;
        else if (cmd >= 0xB8 && cmd <= 0xBF && cmd == 0xB8) g_ge_stats.tex_sizes[arg]++;
        else if (cmd == 0xD3) g_ge_stats.clear_args[arg]++;
        else if (cmd == 0x9C) g_ge_stats.fb_addrs[arg]++;
        else if (cmd == 0xA8) g_ge_stats.tex_region[(arg >> 16) & 0xF]++;   // obere Adressbits: 4 = VRAM (FB als Textur?), 8/9 = RAM
        switch (cmd) {
        case 0x08: pc = ((base | arg) + offset) & 0x0FFFFFFC; break;                     // JUMP
        case 0x0A:                                                                       // CALL
            if (sp < 32) stack[sp++] = pc;
            pc = ((base | arg) + offset) & 0x0FFFFFFC;
            break;
        case 0x0B: if (sp) pc = stack[--sp]; break;                                      // RET
        case 0x0C: if (end_pc) *end_pc = pc; return finished ? finish : -1;              // END
        case 0x0F: finish = (int)(arg & 0xFFFF); finished = true; break;                 // FINISH
        case 0x10: base = (arg & 0xFF0000) << 8; break;                                  // BASE
        case 0x13: offset = arg << 8; break;                                             // OFFSET_ADDR
        case 0x14: offset = pc - 4; break;                                               // ORIGIN
        default: ge_soft_cmd(mem, w, base); break;
        }
    }
    if (end_pc) *end_pc = pc;
    return finished ? finish : -1;
}

} // namespace

extern "C" {

// ---------------------------------------------------------------- sceGe_user

PSP_HLE(sceGe_user, 0xE47E40E4) { (void)mem; HLE_RET(PSP_VRAM_ADDR); }   // sceGeEdramGetAddr
PSP_HLE(sceGe_user, 0x1F6752AD) { (void)mem; HLE_RET(PSP_VRAM_SIZE); }   // sceGeEdramGetSize

// int sceGeSetCallback(PspGeCallbackData* cb): { signal_func, signal_arg, finish_func, finish_arg }
PSP_HLE(sceGe_user, 0xA4FC06A4) {
    const uint32_t p = HLE_ARG(0);
    if (!mem_valid(p, 16)) { HLE_RET(0x80000023u); return; }
    for (int i = 0; i < 16; i++)
        if (!g_ge_cb[i].used) {
            g_ge_cb[i] = {psp_r32(mem, p), psp_r32(mem, p + 4), psp_r32(mem, p + 8), psp_r32(mem, p + 12), true};
            psp_log(PSP_LOG_INFO, "GE", "callback %d signal=0x%08X finish=0x%08X", i, g_ge_cb[i].signal_func, g_ge_cb[i].finish_func);
            HLE_RET(i);
            return;
        }
    HLE_RET(0x80000022u);
}

// int sceGeUnsetCallback(int id)
PSP_HLE(sceGe_user, 0x05DB22CE) {
    (void)mem;
    const uint32_t id = HLE_ARG(0);
    if (id < 16) g_ge_cb[id] = GeCallback{};
    HLE_RET(0);
}

// int sceGeListEnQueue(const void* list, void* stall, int cbid, PspGeListArgs* arg)
PSP_HLE(sceGe_user, 0xAB49E76A) {
    const uint32_t list = HLE_ARG(0), stall = HLE_ARG(1);
    const int cbid = (int)HLE_ARG(2);
    uint32_t end_pc = 0;
    const uint64_t t0 = time_us();
    const int finish = walk_display_list(mem, list & 0x0FFFFFFF, stall & 0x0FFFFFFF, &end_pc);
    g_perf_ge_us += time_us() - t0;
    const int id = g_ge_list_id++;
    if (++g_ge_stats.lists % 30000 == 0) log_ge_stats();   // selten, Log kostet
    psp_log(PSP_LOG_DEBUG, "GE", "list 0x%08X stall=0x%08X cb=%d -> id=0x%X (gezeichnet: ge_soft.cpp)", list, stall, cbid, id);
    // Liste gilt sofort als fertig, FINISH-Callback wie GE-Interrupt
    if (finish >= 0 && cbid >= 0 && cbid < 16 && g_ge_cb[cbid].used && g_ge_cb[cbid].finish_func)
        intr_queue_call(g_ge_cb[cbid].finish_func, (uint32_t)finish, g_ge_cb[cbid].finish_arg, end_pc);
    HLE_RET(id);
    thread_yield();   // Interrupt durchlassen
}

PSP_HLE(sceGe_user, 0xB287BD61) { (void)mem; HLE_RET(0); }   // sceGeDrawSync, alle Listen schon fertig

// ---------------------------------------------------------------- sceDisplay

PSP_HLE(sceDisplay, 0x0E20F177) {   // sceDisplaySetMode(mode, width, height)
    (void)mem;
    psp_log(PSP_LOG_INFO, "GE", "display mode %u %ux%u", HLE_ARG(0), HLE_ARG(1), HLE_ARG(2));
    HLE_RET(0);
}

PSP_HLE(sceDisplay, 0x289D82FE) {   // sceDisplaySetFrameBuf(addr, stride, format, sync)
    (void)mem;
    g_framebuf = HLE_ARG(0);
    display_set_framebuf(HLE_ARG(0), HLE_ARG(1), HLE_ARG(2));
    HLE_RET(0);
}

PSP_HLE(sceDisplay, 0x984C27E7) { (void)mem; wait_vblank(); HLE_RET(0); }   // sceDisplayWaitVblankStart
PSP_HLE(sceDisplay, 0x46F186C3) { (void)mem; wait_vblank(); HLE_RET(0); }   // sceDisplayWaitVblankStartCB
PSP_HLE(sceDisplay, 0x9C6EAAD7) { (void)mem; HLE_RET(vblank_count()); }     // sceDisplayGetVcount

// ---------------------------------------------------------------- sceCtrl

PSP_HLE(sceCtrl, 0x1F4011E6) { (void)mem; HLE_RET(0); }   // sceCtrlSetSamplingMode, gibt alten Modus zurück

// int sceCtrlPeekBufferPositive(SceCtrlData* data, int count): { u32 timestamp; u32 buttons; u8 lx, ly; u8 rsrv[6] }
PSP_HLE(sceCtrl, 0x3A622550) {
    const uint32_t p = HLE_ARG(0);
    if (!mem_valid(p, 16)) { HLE_RET(0x80000023u); return; }
    psp_w32(mem, p, (uint32_t)time_us());
    psp_w32(mem, p + 4, ctrl_buttons());
    psp_w32(mem, p + 8, 0x00008080u);
    psp_w32(mem, p + 12, 0);
    HLE_RET(1);
}

// ---------------------------------------------------------------- sceAudio (ohne Ausgabe, nur Takt)

// int sceAudioChReserve(int channel, int sampleCount, int format)
PSP_HLE(sceAudio, 0x5EC81C55) {
    (void)mem;
    int ch = (int)HLE_ARG(0);
    if (ch < 0) {
        for (ch = 7; ch >= 0 && g_audio[ch].reserved; ch--) {}
        if (ch < 0) { HLE_RET(0x80260005u); return; }   // kein Kanal frei
    }
    if (ch >= 8) { HLE_RET(0x80260003u); return; }
    g_audio[ch] = {true, HLE_ARG(1), 0, HLE_ARG(2) == 0x10};
    HLE_RET(ch);
}

PSP_HLE(sceAudio, 0x6FC46853) {   // sceAudioChRelease(channel)
    (void)mem;
    const uint32_t ch = HLE_ARG(0);
    if (ch >= 8 || !g_audio[ch].reserved) { HLE_RET(0x80260008u); return; }
    g_audio[ch] = AudioChannel{};
    HLE_RET(0);
}

static void audio_output(PspContext* ctx, uint8_t* mem, int vl, int vr, uint32_t buf) {
    const uint32_t ch = HLE_ARG(0);
    if (ch >= 8 || !g_audio[ch].reserved) { HLE_RET(0x80260003u); return; }
    const uint32_t samples = g_audio[ch].samples;
    audio_submit((int)ch, mem, buf, samples, g_audio[ch].mono, vl, vr);
    audio_pace(g_audio[ch], samples, 44100);
    HLE_RET(samples);
}
PSP_HLE(sceAudio, 0x136CAF51) { audio_output(ctx, mem, (int)HLE_ARG(1), (int)HLE_ARG(1), HLE_ARG(2)); }   // sceAudioOutputBlocking(channel, vol, buf)
PSP_HLE(sceAudio, 0x13F592BC) { audio_output(ctx, mem, (int)HLE_ARG(1), (int)HLE_ARG(2), HLE_ARG(3)); }   // sceAudioOutputPannedBlocking(channel, l, r, buf)

PSP_HLE(sceAudio, 0x95FD0C2D) { (void)mem; HLE_RET(0); }          // sceAudioChangeChannelConfig
PSP_HLE(sceAudio, 0xB011922F) { (void)mem; psp_log(PSP_LOG_VERBOSE, "AUDIO", "GetChannelRestLength ch=%u -> 0", HLE_ARG(0)); HLE_RET(0); }          // sceAudioGetChannelRestLength
PSP_HLE(sceAudio, 0xCB2E439E) {                                   // sceAudioSetChannelDataLen(channel, samples)
    (void)mem;
    const uint32_t ch = HLE_ARG(0);
    if (ch >= 8 || !g_audio[ch].reserved) { HLE_RET(0x80260003u); return; }
    g_audio[ch].samples = HLE_ARG(1);
    HLE_RET(0);
}

PSP_HLE(sceAudio, 0x01562BA3) { (void)mem; g_output2 = {true, HLE_ARG(0), 0}; HLE_RET(0); }   // sceAudioOutput2Reserve(samples)
PSP_HLE(sceAudio, 0x43196845) { (void)mem; g_output2 = AudioChannel{}; HLE_RET(0); }          // sceAudioOutput2Release
PSP_HLE(sceAudio, 0x2D53F36E) {                                                               // sceAudioOutput2OutputBlocking(vol, buf)
    (void)mem;
    if (!g_output2.reserved) { HLE_RET(0x80260008u); return; }
    const uint32_t samples = g_output2.samples;
    audio_submit(8, mem, HLE_ARG(1), samples, false, (int)HLE_ARG(0), (int)HLE_ARG(0));
    audio_pace(g_output2, samples, 44100);
    HLE_RET(samples);
}

// ---------------------------------------------------------------- sceVaudio

PSP_HLE(sceVaudio, 0x03B6807D) {   // sceVaudioChReserve(samples, freq, channels)
    (void)mem;
    g_vaudio = {true, HLE_ARG(0), 0, HLE_ARG(2) == 1};
    g_vaudio_freq = HLE_ARG(1) ? HLE_ARG(1) : 44100;
    HLE_RET(0);
}
PSP_HLE(sceVaudio, 0x67585DFD) { (void)mem; g_vaudio = AudioChannel{}; HLE_RET(0); }   // sceVaudioChRelease
PSP_HLE(sceVaudio, 0x8986295E) {                                                       // sceVaudioOutputBlocking(vol, buf)
    (void)mem;
    if (!g_vaudio.reserved) { HLE_RET(0x80260008u); return; }
    audio_submit(9, mem, HLE_ARG(1), g_vaudio.samples, g_vaudio.mono, (int)HLE_ARG(0), (int)HLE_ARG(0));
    audio_pace(g_vaudio, g_vaudio.samples, g_vaudio_freq);
    HLE_RET(0);
}

// ---------------------------------------------------------------- sceRtc

static uint64_t rtc_ticks() {   // Mikrosekunden seit 1.1.0001
    timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ((uint64_t)ts.tv_sec + 62135596800ull) * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

// int sceRtcGetCurrentClockLocalTime(pspTime*): { u16 year, month, day, hour, minute, second; u32 microsecond }
PSP_HLE(sceRtc, 0xE7C27D1B) {
    const uint32_t p = HLE_ARG(0);
    if (!mem_valid(p, 16)) { HLE_RET(0x80000023u); return; }
    const time_t now = time(nullptr);
    tm lt;
    localtime_s(&lt, &now);
    psp_w16(mem, p, (uint16_t)(lt.tm_year + 1900));
    psp_w16(mem, p + 2, (uint16_t)(lt.tm_mon + 1));
    psp_w16(mem, p + 4, (uint16_t)lt.tm_mday);
    psp_w16(mem, p + 6, (uint16_t)lt.tm_hour);
    psp_w16(mem, p + 8, (uint16_t)lt.tm_min);
    psp_w16(mem, p + 10, (uint16_t)lt.tm_sec);
    psp_w32(mem, p + 12, 0);
    HLE_RET(0);
}

PSP_HLE(sceRtc, 0x011F03C1) { (void)mem; HLE_RET64(rtc_ticks()); }   // sceRtcGetAccumulativeTime

// int sceRtcGetTime64_t(const pspTime* date, u64* time): Sekunden seit 1970
PSP_HLE(sceRtc, 0xE1C93E47) {
    const uint32_t p = HLE_ARG(0), out = HLE_ARG(1);
    if (!mem_valid(p, 16) || !mem_valid(out, 8)) { HLE_RET(0x80000023u); return; }
    tm t{};
    t.tm_year = psp_r16(mem, p) - 1900;
    t.tm_mon = psp_r16(mem, p + 2) - 1;
    t.tm_mday = psp_r16(mem, p + 4);
    t.tm_hour = psp_r16(mem, p + 6);
    t.tm_min = psp_r16(mem, p + 8);
    t.tm_sec = psp_r16(mem, p + 10);
    const int64_t secs = (int64_t)_mkgmtime(&t);
    psp_w32(mem, out, (uint32_t)secs);
    psp_w32(mem, out + 4, (uint32_t)((uint64_t)secs >> 32));
    HLE_RET(0);
}

} // extern "C"
