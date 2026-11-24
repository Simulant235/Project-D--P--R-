// Threads und Zeit, erste Funktionen aus T20 (Rest in stubs/)
// Fehlercodes nach PPSSPP (Core/HLE/sceKernelThread.cpp)
#include "../src/rt.h"

using namespace rt;

namespace {
const uint32_t SCE_KERNEL_ERROR_ERROR = 0x80020001u;
const uint32_t SCE_KERNEL_ERROR_UNKNOWN_THID = 0x80020198u;
const uint32_t SCE_KERNEL_ERROR_WAIT_TIMEOUT = 0x800201A8u;
const uint32_t SCE_KERNEL_ERROR_ILLEGAL_ADDR = 0x800200D3u;
} // namespace

extern "C" {

// int sceKernelCreateThread(const char* name, entry, int initPriority, int stackSize, u32 attr, option*)
PSP_HLE(ThreadManForUser, 0x446D8DE6) {
    const uint32_t name = HLE_ARG(0), entry = HLE_ARG(1);
    if (!name || !mem_valid(name, 1)) { HLE_RET(SCE_KERNEL_ERROR_ERROR); return; }
    if (!mem_valid(entry, 4)) { HLE_RET(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    HLE_RET(thread_create(HLE_STR(name), entry, (int)HLE_ARG(2), HLE_ARG(3), HLE_ARG(4)));
}

// int sceKernelStartThread(SceUID id, int argSize, void* argp)
PSP_HLE(ThreadManForUser, 0xF475845D) {
    (void)mem;
    uint32_t len = HLE_ARG(1), ptr = HLE_ARG(2);
    if (len && !mem_valid(ptr, len)) len = 0;
    ctx->r[PSP_V0] = 0;   // Thread kann sofort laufen, Ergebnis steht schon im Register
    const int result = thread_start((int)HLE_ARG(0), len, ptr);
    HLE_RET(result);
}

// int sceKernelDelayThread(u32 usec)
PSP_HLE(ThreadManForUser, 0xCEADEB47) {
    (void)mem;
    const uint32_t usec = HLE_ARG(0);
    thread_block(WaitType::Delay, 0, usec < 200 ? 200 : usec);
    HLE_RET(0);
}

// int sceKernelDelayThreadCB(u32 usec)   (Callbacks: T24)
PSP_HLE(ThreadManForUser, 0x68DA9E36) {
    (void)mem;
    const uint32_t usec = HLE_ARG(0);
    thread_block(WaitType::Delay, 0, usec < 200 ? 200 : usec);
    HLE_RET(0);
}

// void sceKernelExitThread(int status)
PSP_HLE(ThreadManForUser, 0xAA73C935) {
    (void)mem;
    thread_exit((int)HLE_ARG(0), false);
}

// void sceKernelExitDeleteThread(int status)
PSP_HLE(ThreadManForUser, 0x809CE29B) {
    (void)mem;
    thread_exit((int)HLE_ARG(0), true);
}

// int sceKernelDeleteThread(SceUID id)
PSP_HLE(ThreadManForUser, 0x9FA03CD3) {
    (void)mem;
    HLE_RET(thread_delete((int)HLE_ARG(0)));
}

// int sceKernelWaitThreadEnd(SceUID id, u32* timeout)
PSP_HLE(ThreadManForUser, 0x278C0DF5) {
    const int id = (int)HLE_ARG(0);
    const uint32_t timeout_ptr = HLE_ARG(1);
    Thread* t = thread_by_id(id);
    if (!t || t == cur()) { HLE_RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->state == ThreadState::Dead || t->state == ThreadState::Dormant) { HLE_RET(0); return; }
    const uint64_t timeout = timeout_ptr && mem_valid(timeout_ptr, 4) ? psp_r32(mem, timeout_ptr) : 0;
    thread_block(WaitType::ThreadEnd, (uint32_t)id, timeout);
    HLE_RET(cur()->timed_out ? SCE_KERNEL_ERROR_WAIT_TIMEOUT : 0);
}

// SceUID sceKernelGetThreadId(void)
PSP_HLE(ThreadManForUser, 0x293B45B8) {
    (void)mem;
    HLE_RET(cur()->id);
}

// int sceKernelGetThreadCurrentPriority(void)
PSP_HLE(ThreadManForUser, 0x94AA61EE) {
    (void)mem;
    HLE_RET(cur()->priority);
}

// u64 sceKernelGetSystemTimeWide(void)
PSP_HLE(ThreadManForUser, 0x82BC5777) {
    (void)mem;
    HLE_RET64(time_us());
}

// u32 sceKernelGetSystemTimeLow(void)
PSP_HLE(ThreadManForUser, 0x369ED59D) {
    (void)mem;
    HLE_RET((uint32_t)time_us());
}

} // extern "C"
