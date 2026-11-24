// waveOut-Ausgabe (T22): pro Kanal eine Queue (16 Bit Stereo), ein Host-Thread mischt mit 44,1 kHz.
// Takt fürs Spiel kommt weiter aus audio_pace() in devices_placeholder.cpp.
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#include "../src/rt.h"
#include "audio_out.h"

using namespace rt;

namespace {

const int SLOTS = 11;                       // 8 sceAudio-Kanäle, Output2, Vaudio, Reserve
const int RATE = 44100, FRAMES = 1024, BUFS = 4;
const size_t MAX_QUEUE = RATE / 3;          // max. 0,33 s pro Queue, älteste Samples fliegen raus
std::mutex g_lock;
std::deque<int16_t> g_q[SLOTS];             // L, R abwechselnd
std::atomic<bool> g_started{false};
std::atomic<bool> g_frozen{false};
std::thread g_thread;

void player() {
    rt::name_thread(L"Ton");
    HWAVEOUT dev = nullptr;
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 2; fmt.nSamplesPerSec = RATE; fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4; fmt.nAvgBytesPerSec = RATE * 4;
    if (waveOutOpen(&dev, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        psp_log(PSP_LOG_WARN, "AUDIO", "waveOutOpen fehlgeschlagen, kein Ton");
        return;
    }
    static int16_t data[BUFS][FRAMES * 2];
    WAVEHDR hdr[BUFS] = {};
    for (int i = 0; i < BUFS; i++) {
        hdr[i].lpData = (LPSTR)data[i];
        hdr[i].dwBufferLength = sizeof data[i];
        waveOutPrepareHeader(dev, &hdr[i], sizeof hdr[i]);
        hdr[i].dwFlags |= WHDR_DONE;
    }
    for (;;) {
        for (int i = 0; i < BUFS; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            if (!g_frozen) {
                std::lock_guard<std::mutex> lk(g_lock);
                for (int k = 0; k < FRAMES * 2; k++) {
                    int sum = 0;
                    for (auto& q : g_q)
                        if (!q.empty()) { sum += q.front(); q.pop_front(); }
                    data[i][k] = (int16_t)(sum < -32768 ? -32768 : sum > 32767 ? 32767 : sum);
                }
            }
            hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(dev, &hdr[i], sizeof hdr[i]);
        }
        Sleep(5);
    }
}

} // namespace

void audio_freeze() { g_frozen = true; }

int audio_queued_ms() {
    std::lock_guard<std::mutex> lk(g_lock);
    size_t m = 0;
    for (auto& q : g_q) m = std::max(m, q.size() / 2);
    return (int)((m + BUFS * FRAMES) * 1000 / RATE);
}

void audio_submit(int slot, const uint8_t* mem, uint32_t buf, uint32_t samples, bool mono, int vol_l, int vol_r) {
    if (slot < 0 || slot >= SLOTS || !samples || !buf) return;
    if (!mem_valid(buf, samples * (mono ? 2 : 4))) return;
    const int16_t* p = (const int16_t*)(mem + buf);
    {   // Statistik alle 10 s (Menge, Lautstärke)
        static uint64_t next = 10000000, count = 0;
        static int peak = 0;
        count += samples;
        for (uint32_t i = 0; i < samples * (mono ? 1 : 2); i++) peak = std::max(peak, std::abs((int)p[i]));
        if (time_us() >= next) {
            psp_log(PSP_LOG_INFO, "AUDIO", "Slot %d: %llu Samples in 10 s (Soll 441000), Spitze %d", slot, (unsigned long long)count, peak);
            next = time_us() + 10000000;
            count = 0;
            peak = 0;
        }
    }
    if (g_opt.input_log) {
        int pk = 0;
        for (uint32_t i = 0; i < samples * (mono ? 1 : 2); i++) pk = std::max(pk, std::abs((int)p[i]));
        rt::input_log_sound(slot, pk);
    }
    if (g_opt.mute) return;
    if (!g_started.exchange(true)) { g_thread = std::thread(player); g_thread.detach(); }
    std::lock_guard<std::mutex> lk(g_lock);
    auto& q = g_q[slot];
    for (uint32_t i = 0; i < samples; i++) {
        const int l = mono ? p[i] : p[i * 2], r = mono ? p[i] : p[i * 2 + 1];
        q.push_back((int16_t)(l * vol_l / 0x8000));
        q.push_back((int16_t)(r * vol_r / 0x8000));
    }
    if (q.size() > MAX_QUEUE * 2) q.erase(q.begin(), q.begin() + (ptrdiff_t)(q.size() - MAX_QUEUE * 2));
}
