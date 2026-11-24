// sceDmac (T24): Speicherkopie, u.a. fertige Videobilder ins VRAM
#include <cstring>

#include "../src/rt.h"

extern "C" {

// int sceDmacMemcpy(void* dst, const void* src, u32 size)
PSP_HLE(sceDmac, 0x617F3FE6) {
    const uint32_t dst = HLE_ARG(0), src = HLE_ARG(1), size = HLE_ARG(2);
    if (size == 0) { HLE_RET(0); return; }
    if (!rt::mem_valid(dst, size) || !rt::mem_valid(src, size)) { HLE_RET(0x800200D3u); return; }   // ILLEGAL_ADDR
    memmove(mem + dst, mem + src, size);
    HLE_RET(0);
}

} // extern "C"
