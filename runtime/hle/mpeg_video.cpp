// H.264 der PSMF-Videos: Ringpuffer-Pakete entpacken (Video 0xE0), Annex-B an den MF-Decoder
// (CLSID_CMSH264DecoderMFT), NV12 -> RGBA 8888. Ablauf/Formate nach PPSSPP Core/HW/MediaEngine.cpp.
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "../src/rt.h"
#include "mpeg_video.h"
#include "at3_decoders.h"

using namespace rt;

namespace {

std::deque<uint8_t> g_es;                 // Video-Bytestrom, noch nicht im Decoder
std::deque<uint8_t> g_aes;                // Audio-Bytestrom (ATRAC3plus-Rahmen mit 8-Byte-Kopf 0F D0 ..)
ATRAC3PContext* g_at3 = nullptr;
int g_at3_channels = 0;
IMFTransform* g_dec = nullptr;
bool g_mf_ok = false, g_mf_tried = false;
UINT32 g_w = 0, g_h = 0, g_stride = 0;    // Ausgabeformat Decoder
std::vector<uint8_t> g_frame;             // letztes Bild (NV12)
bool g_have_frame = false;

bool set_output_type() {
    for (DWORD i = 0;; i++) {
        IMFMediaType* t = nullptr;
        if (FAILED(g_dec->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub = {};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            const HRESULT hr = g_dec->SetOutputType(0, t, 0);
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &g_w, &g_h);
            g_stride = MFGetAttributeUINT32(t, MF_MT_DEFAULT_STRIDE, g_w);
            t->Release();
            psp_log(PSP_LOG_INFO, "MPEG", "H.264-Decoder: Ausgabe NV12 %ux%u stride %u", g_w, g_h, g_stride);
            return SUCCEEDED(hr);
        }
        t->Release();
    }
}

bool init_decoder() {
    if (g_mf_tried) return g_mf_ok;
    g_mf_tried = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_dec)))) {
        psp_log(PSP_LOG_WARN, "MPEG", "H.264-Decoder von Windows nicht verfügbar");
        return false;
    }
    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, 480, 272);
    const HRESULT hr = g_dec->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr) || !set_output_type()) {
        psp_log(PSP_LOG_WARN, "MPEG", "H.264-Decoder: Formate abgelehnt (0x%08lX)", (unsigned long)hr);
        return false;
    }
    g_dec->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    g_dec->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    g_mf_ok = true;
    return true;
}

// fertiges Bild holen, falls vorhanden
bool pull_frame() {
    MFT_OUTPUT_STREAM_INFO info = {};
    g_dec->GetOutputStreamInfo(0, &info);
    IMFSample* s = nullptr;
    IMFMediaBuffer* b = nullptr;
    MFCreateSample(&s);
    MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, g_stride * g_h * 3 / 2), &b);
    s->AddBuffer(b);
    MFT_OUTPUT_DATA_BUFFER out = {};
    out.pSample = s;
    DWORD status = 0;
    HRESULT hr = g_dec->ProcessOutput(0, 1, &out, &status);
    if (out.pEvents) out.pEvents->Release();
    bool got = false;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        set_output_type();
    } else if (SUCCEEDED(hr)) {
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
            g_frame.assign(p, p + len);
            b->Unlock();
            got = g_have_frame = true;
        }
    }
    b->Release();
    s->Release();
    return got;
}

bool push_input(size_t n) {
    IMFSample* s = nullptr;
    IMFMediaBuffer* b = nullptr;
    MFCreateSample(&s);
    MFCreateMemoryBuffer((DWORD)n, &b);
    BYTE* p = nullptr;
    b->Lock(&p, nullptr, nullptr);
    std::copy(g_es.begin(), g_es.begin() + (ptrdiff_t)n, p);
    b->Unlock();
    b->SetCurrentLength((DWORD)n);
    s->AddBuffer(b);
    const HRESULT hr = g_dec->ProcessInput(0, s, 0);
    b->Release();
    s->Release();
    if (hr == MF_E_NOTACCEPTING) return false;
    g_es.erase(g_es.begin(), g_es.begin() + (ptrdiff_t)n);
    return true;
}

uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

} // namespace

void mpegv_reset() {
    g_es.clear();
    g_aes.clear();
    if (g_at3) atrac3p_flush_buffers(g_at3);
    g_have_frame = false;
    if (g_dec) {
        g_dec->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        g_dec->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
}

// 2048-Byte-Pakete (MPEG-PS: Pack-Kopf, PES) entpacken, nur Video 0xE0 bleibt
void mpegv_feed_packets(const uint8_t* data, int packets) {
    for (int k = 0; k < packets; k++) {
        const uint8_t* p = data + (size_t)k * 2048;
        const uint8_t* end = p + 2048;
        if (p[0] != 0 || p[1] != 0 || p[2] != 1 || p[3] != 0xBA) continue;
        p += 14 + (p[13] & 7);
        while (p + 6 <= end && p[0] == 0 && p[1] == 0 && p[2] == 1) {
            const uint8_t sid = p[3];
            const size_t len = ((size_t)p[4] << 8) | p[5];
            const uint8_t* body = p + 6;
            if (body + len > end) break;
            if (sid == 0xE0 && len >= 3) {
                const size_t hdr = 3 + body[2];
                if (hdr <= len) g_es.insert(g_es.end(), body + hdr, body + len);
            } else if (sid == 0xBD && len >= 3) {   // privater Strom 1: Kanalbyte + 3 Byte Unterkopf, dann AT3+
                const size_t hdr = 3 + body[2];
                if (hdr + 4 <= len && body[hdr] == 0x00) g_aes.insert(g_aes.end(), body + hdr + 4, body + len);
            }
            p = body + len;
        }
    }
}

// Bild dekodieren, RGBA 8888 (Breite 480, Zeilenabstand fw Pixel) nach dst
bool mpegv_decode(uint8_t* mem, uint32_t dst, uint32_t fw) {
    if (!init_decoder()) return false;
    bool got = pull_frame();
    while (!got && !g_es.empty()) {
        if (!push_input(std::min<size_t>(g_es.size(), 4096))) {
            got = pull_frame();
            if (!got) break;
        } else {
            got = pull_frame();
        }
    }
    if (!g_have_frame || !fw || !g_w || !g_h) return got;
    const uint32_t w = std::min<uint32_t>(g_w, std::min<uint32_t>(fw, 480)), h = std::min<uint32_t>(g_h, 272);
    if (!mem_valid(dst, (fw * (h - 1) + w) * 4) || g_frame.size() < (size_t)g_stride * g_h * 3 / 2) return got;
    const uint8_t* yp = g_frame.data();
    const uint8_t* uvp = yp + (size_t)g_stride * g_h;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* out = mem + dst + (size_t)y * fw * 4;
        const uint8_t* yr = yp + (size_t)y * g_stride;
        const uint8_t* uvr = uvp + (size_t)(y / 2) * g_stride;
        for (uint32_t x = 0; x < w; x++) {
            const int c = yr[x] - 16, d = uvr[x & ~1u] - 128, e = uvr[(x & ~1u) + 1] - 128;
            out[x * 4 + 0] = clamp8((298 * c + 409 * e + 128) >> 8);
            out[x * 4 + 1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            out[x * 4 + 2] = clamp8((298 * c + 516 * d + 128) >> 8);
            out[x * 4 + 3] = 255;
        }
    }
    return got;
}

// AT3+-Rahmen -> 2048 Samples 16 Bit Stereo (8192 Byte) nach buf, false = keine Daten
bool mpega_decode(uint8_t* mem, uint32_t buf) {
    // Rahmenanfang 0F D0 suchen
    while (g_aes.size() >= 2 && !(g_aes[0] == 0x0F && g_aes[1] == 0xD0)) g_aes.pop_front();
    if (g_aes.size() < 8) return false;
    const uint8_t c1 = g_aes[2], c2 = g_aes[3];
    const size_t frame = ((((size_t)c1 & 3) << 8) | ((size_t)c2 * 8)) + 0x10;
    if (g_aes.size() < frame) return false;
    std::vector<uint8_t> data(g_aes.begin() + 8, g_aes.begin() + (ptrdiff_t)frame);
    g_aes.erase(g_aes.begin(), g_aes.begin() + (ptrdiff_t)frame);
    const int channels = c1 == 0x24 ? 1 : 2;
    if (!g_at3 || channels != g_at3_channels) {
        if (g_at3) atrac3p_free(g_at3);
        int align = (int)data.size();
        g_at3 = atrac3p_alloc(channels, &align);
        g_at3_channels = channels;
        if (!g_at3) return false;
    }
    static float l[4096], r[4096];
    float* out[2] = {l, r};
    int n = 0;
    if (atrac3p_decode_frame(g_at3, out, &n, data.data(), (int)data.size()) < 0 || n <= 0) return false;
    n = std::min(n, 2048);
    if (static FILE* dump = getenv("DIVA_DUMP_AT3") ? fopen(getenv("DIVA_DUMP_AT3"), "wb") : nullptr; dump) {   // Dump zum Prüfen
        for (int i = 0; i < n; i++) { const float v = l[i] * 32767.f; const int16_t x = (int16_t)std::clamp(v, -32767.f, 32767.f); fwrite(&x, 2, 1, dump); }
        fflush(dump);
    }
    auto q = [](float f) { return (int16_t)(f >= 1.f ? 32767 : f <= -1.f ? -32767 : (int)(f * 32767.f)); };
    for (int i = 0; i < 2048; i++) {
        const int16_t a = i < n ? q(l[i]) : 0, b = i < n ? q(channels == 2 ? r[i] : l[i]) : 0;
        psp_w16(mem, buf + (uint32_t)i * 4, (uint16_t)a);
        psp_w16(mem, buf + (uint32_t)i * 4 + 2, (uint16_t)b);
    }
    return true;
}
