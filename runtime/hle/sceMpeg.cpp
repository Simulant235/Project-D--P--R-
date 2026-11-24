// sceMpeg (T24), vorläufig: Bild über MF (mpeg_video.cpp), Ton noch Stille.
// Verwaltung (Ringpuffer, Handles, PSMF-Kopf) nach PPSSPP Core/HLE/sceMpeg.cpp, damit Sofdec die Checks besteht.
// Fehlercodes nach PPSSPP Core/HLE/ErrorCodes.h. TODO: AT3+ für den Ton
#include <algorithm>
#include <cstring>
#include <map>

#include "../src/rt.h"
#include "mpeg_video.h"

using namespace rt;

namespace {

enum : uint32_t {
    ERR_NO_DATA = 0x80618001u, ERR_BAD_VERSION = 0x80610002u, ERR_NO_MEMORY = 0x80610022u,
    ERR_INVALID_VALUE = 0x806101FEu, ERR_NOT_YET_INIT = 0x80618009u, ERR_ILLEGAL_ADDRESS = 0x800200D3u
};
const uint32_t MEMSIZE = 0x10000, PSMF_MAGIC = 0x464D5350, AVC_ES_SIZE = 2048, ATRAC_ES_SIZE = 2112;
const int ES_BUFFERS = 2;

struct Ctx {
    uint32_t ring = 0;
    int next_stream = 1;
    bool es[ES_BUFFERS] = {};
    int64_t first_ts = 0, last_ts = 0, video_time = 0, audio_time = 0;   // aus PSMF-Kopf, video_time = bisher gespielt (90 kHz)
    bool analyzed = false;
    uint32_t frames = 0, width = 480, height = 272;
};
const int64_t AUDIO_STEP = 4180;   // 2048 Samples bei 44,1 kHz in 90-kHz-Ticks
const int64_t VIDEO_STEP = 3003;   // 29,97 Bilder pro Sekunde
std::map<uint32_t, Ctx> g_ctx;   // Handle (mpegData + 0x30) -> Kontext
bool g_init = false;

Ctx* get(uint8_t* mem, uint32_t mpeg) {
    if (!mem_valid(mpeg, 4)) return nullptr;
    auto it = g_ctx.find(psp_r32(mem, mpeg));   // *mpeg = Handle (siehe sceMpegCreate)
    return it == g_ctx.end() ? nullptr : &it->second;
}

// SceMpegRingBuffer: packets, packetsRead, packetsWritePos, packetsAvail, packetSize, data, callback, callback_args,
// dataUpperBound, semaID, mpeg, gp
enum { RB_PACKETS = 0, RB_READ = 4, RB_WRITEPOS = 8, RB_AVAIL = 12, RB_SIZE = 16, RB_DATA = 20, RB_CB = 24, RB_CBARG = 28,
       RB_UPPER = 32, RB_SEMA = 36, RB_MPEG = 40, RB_GP = 44 };

int64_t psmf_ts(const uint8_t* p) {   // 5 Bytes ab p+1, big-endian (PPSSPP getMpegTimeStamp)
    return (int64_t)p[5] | ((int64_t)p[4] << 8) | ((int64_t)p[3] << 16) | ((int64_t)p[2] << 24) | ((int64_t)p[1] << 32);
}

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// PSMF-Kopf: "PSMF", Version, Offset des Datenstroms (BE), Größe (BE)
uint32_t analyze(const uint8_t* mem, uint32_t buf, uint32_t* offset, uint32_t* size) {
    if (!mem_valid(buf, 16)) return ERR_ILLEGAL_ADDRESS;
    if (psp_r32(mem, buf) != PSMF_MAGIC) return ERR_INVALID_VALUE;
    *offset = be32(mem + buf + 8);
    *size = be32(mem + buf + 12);
    return 0;
}

void trace(const PspContext* ctx, const char* name) {
    psp_log(PSP_LOG_DEBUG, "HLE", "%s a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X", name, ctx->r[PSP_A0], ctx->r[PSP_A1], ctx->r[PSP_A2], ctx->r[PSP_A3]);
}

} // namespace

extern "C" {

PSP_HLE(sceMpeg, 0x682A619B) { trace(ctx, "sceMpegInit"); (void)mem; g_init = true; HLE_RET(0); }    // sceMpegInit
PSP_HLE(sceMpeg, 0x874624D6) { trace(ctx, "sceMpegFinish"); (void)mem; g_init = false; HLE_RET(0); }   // sceMpegFinish
PSP_HLE(sceMpeg, 0xC132E22F) { trace(ctx, "sceMpegQueryMemSize"); (void)mem; HLE_RET(MEMSIZE); }             // sceMpegQueryMemSize(mode)

// int sceMpegRingbufferQueryMemSize(int packets)
PSP_HLE(sceMpeg, 0xD7A29F46) { trace(ctx, "sceMpegRingbufferQueryMemSize"); (void)mem; HLE_RET((uint32_t)((int32_t)HLE_ARG(0) * (104 + 2048))); }

// int sceMpegRingbufferConstruct(ring, packets, data, size, callback, callbackArg)
PSP_HLE(sceMpeg, 0x37295ED8) { trace(ctx, "sceMpegRingbufferConstruct");
    const uint32_t ring = HLE_ARG(0), packets = HLE_ARG(1), data = HLE_ARG(2), size = HLE_ARG(3);
    if (!mem_valid(ring, 48)) { HLE_RET(ERR_ILLEGAL_ADDRESS); return; }
    if ((int32_t)size < 0 || (packets < 0x00100000 && packets * (104 + 2048) > size)) { HLE_RET(ERR_NO_MEMORY); return; }
    psp_w32(mem, ring + RB_PACKETS, packets);
    psp_w32(mem, ring + RB_READ, 0);
    psp_w32(mem, ring + RB_WRITEPOS, 0);
    psp_w32(mem, ring + RB_AVAIL, 0);
    psp_w32(mem, ring + RB_SIZE, 2048);
    psp_w32(mem, ring + RB_DATA, data);
    psp_w32(mem, ring + RB_CB, HLE_ARG(4));
    psp_w32(mem, ring + RB_CBARG, HLE_ARG(5));
    psp_w32(mem, ring + RB_UPPER, data + packets * 2048);
    psp_w32(mem, ring + RB_MPEG, 0);
    psp_w32(mem, ring + RB_GP, cur() ? cur()->ctx.r[PSP_GP] : 0);
    HLE_RET(0);
}

PSP_HLE(sceMpeg, 0x13407F13) { trace(ctx, "sceMpegRingbufferDestruct"); (void)mem; HLE_RET(0); }   // sceMpegRingbufferDestruct

// int sceMpegCreate(mpeg, mpegData, size, ring, frameWidth, mode, ddrTop)
PSP_HLE(sceMpeg, 0xD8C5F121) { trace(ctx, "sceMpegCreate");
    const uint32_t mpeg = HLE_ARG(0), data = HLE_ARG(1), size = HLE_ARG(2), ring = HLE_ARG(3);
    if (!mem_valid(mpeg, 4) || !mem_valid(data, 0x40)) { HLE_RET(0xFFFFFFFFu); return; }
    if (size < MEMSIZE) { HLE_RET(ERR_NO_MEMORY); return; }
    if (mem_valid(ring, 48)) {
        const uint32_t sz = psp_r32(mem, ring + RB_SIZE);
        psp_w32(mem, ring + RB_AVAIL, sz ? psp_r32(mem, ring + RB_PACKETS) - (psp_r32(mem, ring + RB_UPPER) - psp_r32(mem, ring + RB_DATA)) / sz : 0);
        psp_w32(mem, ring + RB_MPEG, mpeg);
    }
    const uint32_t handle = data + 0x30;
    psp_w32(mem, mpeg, handle);
    memcpy(mem + handle, "LIBMPEG\0", 8);
    memcpy(mem + handle + 8, "001\0", 4);
    psp_w32(mem, handle + 12, 0xFFFFFFFFu);
    if (mem_valid(ring, 48)) {
        psp_w32(mem, handle + 16, ring);
        psp_w32(mem, handle + 20, psp_r32(mem, ring + RB_UPPER));
    }
    Ctx c;
    c.ring = ring;
    g_ctx[handle] = c;
    HLE_RET(0);
}

PSP_HLE(sceMpeg, 0x606A4649) { trace(ctx, "sceMpegDelete");   // sceMpegDelete(mpeg)
    if (!get(mem, HLE_ARG(0))) { HLE_RET(0xFFFFFFFFu); return; }
    g_ctx.erase(psp_r32(mem, HLE_ARG(0)));
    HLE_RET(0);
}

// int sceMpegRegistStream(mpeg, streamType, streamNum): liefert die Stream-ID
PSP_HLE(sceMpeg, 0x42560F23) { trace(ctx, "sceMpegRegistStream");
    Ctx* c = get(mem, HLE_ARG(0));
    if (!c) { HLE_RET(0xFFFFFFFFu); return; }
    HLE_RET((uint32_t)c->next_stream++);
}

PSP_HLE(sceMpeg, 0x591A4AA2) { trace(ctx, "sceMpegUnRegistStream"); Ctx* c = get(mem, HLE_ARG(0)); HLE_RET(c ? 0 : 0xFFFFFFFFu); }   // sceMpegUnRegistStream
PSP_HLE(sceMpeg, 0x707B7629) { trace(ctx, "sceMpegFlushAllStream");                                                                  // sceMpegFlushAllStream
    Ctx* c = get(mem, HLE_ARG(0));
    if (!c) { HLE_RET(0xFFFFFFFFu); return; }
    if (mem_valid(c->ring, 48)) {
        psp_w32(mem, c->ring + RB_AVAIL, 0);
        psp_w32(mem, c->ring + RB_READ, 0);
        psp_w32(mem, c->ring + RB_WRITEPOS, 0);
    }
    HLE_RET(0);
}

// int sceMpegQueryStreamOffset(mpeg, buffer, int* offset)
PSP_HLE(sceMpeg, 0x21FF80E4) { trace(ctx, "sceMpegQueryStreamOffset");
    const uint32_t buf = HLE_ARG(1), out = HLE_ARG(2);
    if (!mem_valid(out, 4) || !get(mem, HLE_ARG(0))) { HLE_RET(0xFFFFFFFFu); return; }
    uint32_t offset = 0, size = 0;
    const uint32_t r = analyze(mem, buf, &offset, &size);
    if (r || (offset & 2047) || offset == 0) {
        psp_w32(mem, out, 0);
        HLE_RET(r ? r : ERR_INVALID_VALUE);
        return;
    }
    Ctx* ctx_m = get(mem, HLE_ARG(0));
    if (mem_valid(buf, 0x60)) {
        ctx_m->first_ts = psmf_ts(mem + buf + 0x54);
        ctx_m->last_ts = psmf_ts(mem + buf + 0x5A);
        ctx_m->video_time = 0;
        ctx_m->audio_time = 0;
        ctx_m->width = *(mem + buf + 142) * 16u;
        ctx_m->height = *(mem + buf + 143) * 16u;
        ctx_m->frames = 0;
        ctx_m->analyzed = true;
        psp_log(PSP_LOG_INFO, "HLE", "psmf: first_ts=%lld last_ts=%lld (%.1f s)", (long long)ctx_m->first_ts, (long long)ctx_m->last_ts,
                (double)(ctx_m->last_ts - ctx_m->first_ts) / 90000.0);
    }
    psp_w32(mem, out, offset);
    mpegv_reset();
    HLE_RET(0);
}

// int sceMpegQueryStreamSize(buffer, int* size)
PSP_HLE(sceMpeg, 0x611E9E11) { trace(ctx, "sceMpegQueryStreamSize");
    const uint32_t buf = HLE_ARG(0), out = HLE_ARG(1);
    if (!mem_valid(out, 4)) { HLE_RET(0xFFFFFFFFu); return; }
    uint32_t offset = 0, size = 0;
    const uint32_t r = analyze(mem, buf, &offset, &size);
    if (r || (offset & 2047)) {
        psp_w32(mem, out, 0);
        HLE_RET(r ? r : ERR_INVALID_VALUE);
        return;
    }
    psp_w32(mem, out, size);
    HLE_RET(0);
}

// int sceMpegRingbufferAvailableSize(ring): freie Pakete
PSP_HLE(sceMpeg, 0xB5F6DC87) { trace(ctx, "sceMpegRingbufferAvailableSize");
    const uint32_t ring = HLE_ARG(0);
    if (!mem_valid(ring, 48)) { HLE_RET(ERR_ILLEGAL_ADDRESS); return; }
    if (!get(mem, psp_r32(mem, ring + RB_MPEG))) { HLE_RET(ERR_NOT_YET_INIT); return; }
    HLE_RET(psp_r32(mem, ring + RB_PACKETS) - psp_r32(mem, ring + RB_AVAIL));
}

// int sceMpegRingbufferPut(ring, numPackets, available): ruft den Füll-Callback des Spiels
PSP_HLE(sceMpeg, 0xB240A59E) { trace(ctx, "sceMpegRingbufferPut");
    const uint32_t ring = HLE_ARG(0);
    int32_t num = (int32_t)HLE_ARG(1), avail = (int32_t)HLE_ARG(2);
    if (!mem_valid(ring, 48)) { HLE_RET(0xFFFFFFFFu); return; }
    const int32_t packets = (int32_t)psp_r32(mem, ring + RB_PACKETS);
    if (packets <= 0) { HLE_RET(ERR_INVALID_VALUE); return; }
    num = std::min(std::min(num, avail), packets - (int32_t)psp_r32(mem, ring + RB_AVAIL));
    if (num <= 0) { HLE_RET(0); return; }
    const uint32_t cb = psp_r32(mem, ring + RB_CB);
    if (!cb) { HLE_RET(0); return; }
    const int32_t write_pos = (int32_t)psp_r32(mem, ring + RB_WRITEPOS) % packets;
    const int32_t now = std::min(num, packets - write_pos);   // bis Pufferende, Rest im nächsten Aufruf
    PspContext saved = *ctx;
    ctx->r[PSP_A0] = psp_r32(mem, ring + RB_DATA) + (uint32_t)write_pos * 2048;
    ctx->r[PSP_A1] = (uint32_t)now;
    ctx->r[PSP_A2] = psp_r32(mem, ring + RB_CBARG);
    psp_call_indirect(ctx, mem, cb);
    const int32_t put = (int32_t)ctx->r[PSP_V0];
    *ctx = saved;
    psp_log(PSP_LOG_VERBOSE, "HLE", "ringbuffer put: callback 0x%08X wanted %d, got %d (avail %u, writepos %u, read %u)", cb, now, put,
            psp_r32(mem, ring + RB_AVAIL), psp_r32(mem, ring + RB_WRITEPOS), psp_r32(mem, ring + RB_READ));
    if (put > 0) {
        mpegv_feed_packets(mem + psp_r32(mem, ring + RB_DATA) + (uint32_t)write_pos * 2048, std::min(put, now));
        psp_w32(mem, ring + RB_WRITEPOS, psp_r32(mem, ring + RB_WRITEPOS) + (uint32_t)put);
        psp_w32(mem, ring + RB_AVAIL, psp_r32(mem, ring + RB_AVAIL) + (uint32_t)put);
        psp_w32(mem, ring + RB_READ, psp_r32(mem, ring + RB_READ) + (uint32_t)put);
    }
    HLE_RET((uint32_t)put);
}

// int sceMpegMallocAvcEsBuf(mpeg): Nummer 1 oder 2, 0 wenn keiner frei
PSP_HLE(sceMpeg, 0xA780CF7E) { trace(ctx, "sceMpegMallocAvcEsBuf");
    Ctx* c = get(mem, HLE_ARG(0));
    if (!c) { HLE_RET(0xFFFFFFFFu); return; }
    for (int i = 0; i < ES_BUFFERS; i++)
        if (!c->es[i]) { c->es[i] = true; HLE_RET((uint32_t)(i + 1)); return; }
    HLE_RET(0);
}

PSP_HLE(sceMpeg, 0xCEB870B1) { trace(ctx, "sceMpegFreeAvcEsBuf");   // sceMpegFreeAvcEsBuf(mpeg, esBuf)
    Ctx* c = get(mem, HLE_ARG(0));
    const uint32_t b = HLE_ARG(1);
    if (!c) { HLE_RET(0xFFFFFFFFu); return; }
    if (b == 0) { HLE_RET(ERR_INVALID_VALUE); return; }
    if (b <= ES_BUFFERS) c->es[b - 1] = false;
    HLE_RET(0);
}

// int sceMpegInitAu(mpeg, esBuffer, SceMpegAu* au): { s64 pts, dts; u32 esBuffer, esSize }
PSP_HLE(sceMpeg, 0x167AFD9E) { trace(ctx, "sceMpegInitAu");
    Ctx* c = get(mem, HLE_ARG(0));
    const uint32_t b = HLE_ARG(1), au = HLE_ARG(2);
    if (!c || !mem_valid(au, 24)) { HLE_RET(0xFFFFFFFFu); return; }
    const bool avc = b >= 1 && b <= ES_BUFFERS && c->es[b - 1];
    psp_w32(mem, au + 16, 0);
    psp_w32(mem, au + 20, avc ? AVC_ES_SIZE : ATRAC_ES_SIZE);
    psp_w32(mem, au + 8, avc ? 0 : 0xFFFFFFFFu);    // dts (Low/High): bei Audio unbekannt
    psp_w32(mem, au + 12, avc ? 0 : 0xFFFFFFFFu);
    psp_w32(mem, au, 0);
    psp_w32(mem, au + 4, 0);
    HLE_RET(0);
}

// int sceMpegQueryAtracEsSize(mpeg, int* esSize, int* outSize)
PSP_HLE(sceMpeg, 0xF8DCB679) { trace(ctx, "sceMpegQueryAtracEsSize");
    const uint32_t es = HLE_ARG(1), out = HLE_ARG(2);
    if (!get(mem, HLE_ARG(0))) { HLE_RET(0xFFFFFFFFu); return; }
    if (mem_valid(es, 4)) psp_w32(mem, es, ATRAC_ES_SIZE);
    if (mem_valid(out, 4)) psp_w32(mem, out, 8192);
    HLE_RET(0);
}

// Keine Daten: Video endet. AUs kommen aus der Videolänge (PSMF-Zeitstempel, 3003 Ticks/Bild), nicht aus dem
// Strom. TODO: echte AUs aus dem Ringpuffer. Jeder Aufruf verbraucht ein paar Pakete.
PSP_HLE(sceMpeg, 0xFE246728) {   // sceMpegGetAvcAu(mpeg, streamId, au*, attr*)
    trace(ctx, "sceMpegGetAvcAu");
    Ctx* c = get(mem, HLE_ARG(0));
    const uint32_t au = HLE_ARG(2), attr = HLE_ARG(3);
    if (!c || !mem_valid(au, 24)) { HLE_RET(0xFFFFFFFFu); return; }
    if (mem_valid(c->ring, 48)) {
        const uint32_t avail = psp_r32(mem, c->ring + RB_AVAIL);
        const uint32_t eat = std::min<uint32_t>(avail, 8);
        psp_w32(mem, c->ring + RB_AVAIL, avail - eat);
    }
    const bool ended = !c->analyzed || c->first_ts + c->video_time >= c->last_ts;
    const int64_t pts = c->first_ts + c->video_time, dts = ended ? -1 : pts - VIDEO_STEP;
    psp_w32(mem, au, (uint32_t)pts);
    psp_w32(mem, au + 4, (uint32_t)((uint64_t)pts >> 32));
    psp_w32(mem, au + 8, (uint32_t)dts);
    psp_w32(mem, au + 12, (uint32_t)((uint64_t)dts >> 32));
    if (ended) { HLE_RET(ERR_NO_DATA); return; }
    if (mem_valid(attr, 4)) psp_w32(mem, attr, 1);
    HLE_RET(0);
}

// int sceMpegAvcDecode(mpeg, au*, frameWidth, void** buffer, int* init): "dekodiert" ein Bild (Pixel kommen aus mpeg_video.cpp)
PSP_HLE(sceMpeg, 0x0E3C2E9D) {
    trace(ctx, "sceMpegAvcDecode");
    Ctx* c = get(mem, HLE_ARG(0));
    const uint32_t au = HLE_ARG(1), init = HLE_ARG(4);
    if (!c || !mem_valid(au, 24) || !mem_valid(init, 4)) { HLE_RET(0xFFFFFFFFu); return; }
    c->video_time += VIDEO_STEP;
    c->frames++;
    const int64_t pts = c->first_ts + c->video_time;
    psp_w32(mem, au, (uint32_t)pts);
    psp_w32(mem, au + 4, (uint32_t)((uint64_t)pts >> 32));
    {   // Bild dekodieren (mpeg_video.cpp), ohne Decoder bleibt das Ziel unverändert
        const uint32_t bp = HLE_ARG(3);
        if (mem_valid(bp, 4)) mpegv_decode(mem, psp_r32(mem, bp), HLE_ARG(2));
    }
    psp_w32(mem, init, 1);   // Bild fertig
    HLE_RET(0);
}

// Audio wie Video: AUs aus der Streamlänge
PSP_HLE(sceMpeg, 0xE1CE83A7) {   // sceMpegGetAtracAu(mpeg, streamId, au*, attr*)
    trace(ctx, "sceMpegGetAtracAu");
    Ctx* c = get(mem, HLE_ARG(0));
    const uint32_t au = HLE_ARG(2), attr = HLE_ARG(3);
    if (!c || !mem_valid(au, 24)) { HLE_RET(0xFFFFFFFFu); return; }
    const bool ended = !c->analyzed || c->first_ts + c->audio_time >= c->last_ts;
    const int64_t pts = ended ? 0 : c->first_ts + c->audio_time;
    psp_w32(mem, au, (uint32_t)pts);
    psp_w32(mem, au + 4, (uint32_t)((uint64_t)pts >> 32));
    psp_w32(mem, au + 8, 0xFFFFFFFFu);   // dts unbekannt
    psp_w32(mem, au + 12, 0xFFFFFFFFu);
    if (ended) { HLE_RET(ERR_NO_DATA); return; }
    if (mem_valid(attr, 4)) psp_w32(mem, attr, 0);
    HLE_RET(0);
}

// int sceMpegAtracDecode(mpeg, au*, buffer, init): liefert 2048 Samples Stille (16 Bit, Stereo)
PSP_HLE(sceMpeg, 0x800C44DF) {
    trace(ctx, "sceMpegAtracDecode");
    Ctx* c = get(mem, HLE_ARG(0));
    const uint32_t au = HLE_ARG(1), buf = HLE_ARG(2);
    if (!c || !mem_valid(buf, 8192) || !mem_valid(au, 24)) { HLE_RET(0xFFFFFFFFu); return; }
    if (!mpega_decode(mem, buf)) memset(mem + buf, 0, 8192);   // Ton aus mpeg_video.cpp, sonst Stille
    c->audio_time += AUDIO_STEP;
    const int64_t pts = c->first_ts + c->audio_time;
    psp_w32(mem, au, (uint32_t)pts);
    psp_w32(mem, au + 4, (uint32_t)((uint64_t)pts >> 32));
    HLE_RET(0);
}

// int sceMpegAvcDecodeDetail(mpeg, detail*)
PSP_HLE(sceMpeg, 0x0F6C18D7) {
    trace(ctx, "sceMpegAvcDecodeDetail");
    Ctx* c = get(mem, HLE_ARG(0));
    const uint32_t d = HLE_ARG(1);
    if (!c || !mem_valid(d, 36)) { HLE_RET(0xFFFFFFFFu); return; }
    psp_w32(mem, d, 0);                // Ergebnis: Erfolg
    psp_w32(mem, d + 4, c->frames);
    psp_w32(mem, d + 8, c->width);
    psp_w32(mem, d + 12, c->height);
    for (uint32_t o = 16; o < 32; o += 4) psp_w32(mem, d + o, 0);
    psp_w32(mem, d + 32, 1);           // Bild fertig
    HLE_RET(0);
}

PSP_HLE(sceMpeg, 0x740FCCD1) { trace(ctx, "sceMpegAvcDecodeStop");   // sceMpegAvcDecodeStop(mpeg, frameWidth, buffer, status*)
    const uint32_t status = HLE_ARG(3);
    if (mem_valid(status, 4)) psp_w32(mem, status, 0);
    HLE_RET(0);
}

PSP_HLE(sceMpeg, 0x4571CC64) { trace(ctx, "sceMpegAvcDecodeFlush"); (void)mem; HLE_RET(0); }   // sceMpegAvcDecodeFlush

} // extern "C"
