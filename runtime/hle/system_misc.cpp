// Kleine Systembibliotheken fürs Booten (T24): SysMem, ModuleMgr, LoadExec, scePower, sceUmdUser, sceImpose,
// Utils, Stdio, sceSuspend, InterruptManager, Callback-Objekte. Rückgabewerte nach PPSSPP (Core/HLE).
#include <ctime>
#include <map>

#include "../src/rt.h"

using namespace rt;

namespace {

const uint32_t SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE = 0x800200D8u;
const uint32_t SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED = 0x800200D9u;
const uint32_t SCE_KERNEL_ERROR_UNKNOWN_UID = 0x800200CBu;
const uint32_t SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT = 0x800200D2u;
const int MODULE_ID = 0x200;

struct MemBlock { uint32_t base, addr, size; };   // base = Blockanfang, addr = ausgerichtet
std::map<int, MemBlock> g_blocks;

struct Callback { std::string name; uint32_t func, arg; int thread; };
std::map<int, Callback> g_callbacks;
int g_exit_callback = 0, g_umd_callback = 0, g_power_callbacks[16] = {};

} // namespace

extern "C" {

// ---------------------------------------------------------------- SysMemUserForUser

// SceUID sceKernelAllocPartitionMemory(int partition, const char* name, int type, u32 size, u32 addrOrAlign)
// type: 0 unten, 1 oben, 2 feste Adresse, 3 unten ausgerichtet, 4 oben ausgerichtet
PSP_HLE(SysMemUserForUser, 0x237DBD4F) {
    const uint32_t name = HLE_ARG(1), type = HLE_ARG(2), size = HLE_ARG(3), extra = HLE_ARG(4);
    const char* n = name && mem_valid(name, 1) ? HLE_STR(name) : "?";
    if (type > 4 || type == 2) {
        psp_log(PSP_LOG_ERROR, "MEM", "sceKernelAllocPartitionMemory '%s': type %u not supported", n, type);
        HLE_RET(SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCKTYPE);
        return;
    }
    uint32_t align = type >= 3 ? extra : 0;
    if (align & (align - 1)) { HLE_RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    if (align <= 0x100) align = 0;
    const bool high = type == 1 || type == 4;
    const uint32_t base = mem_alloc(size + align, high, n);
    if (!base || size == 0) {
        psp_log(PSP_LOG_ERROR, "MEM", "sceKernelAllocPartitionMemory '%s' size=0x%X failed (free 0x%X, max 0x%X)", n, size,
                mem_free_total(), mem_free_max());
        HLE_RET(SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED);
        return;
    }
    const uint32_t addr = align ? (base + align - 1) & ~(align - 1) : base;
    const int uid = new_uid();
    g_blocks[uid] = {base, addr, size};
    psp_log(PSP_LOG_INFO, "MEM", "alloc '%s' uid=0x%X addr=0x%08X size=0x%X type=%u", n, uid, addr, size, type);
    HLE_RET(uid);
}

// int sceKernelFreePartitionMemory(SceUID uid)
PSP_HLE(SysMemUserForUser, 0xB6D61D02) {
    (void)mem;
    auto it = g_blocks.find((int)HLE_ARG(0));
    if (it == g_blocks.end()) { HLE_RET(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    mem_free(it->second.base);
    g_blocks.erase(it);
    HLE_RET(0);
}

// void* sceKernelGetBlockHeadAddr(SceUID uid)
PSP_HLE(SysMemUserForUser, 0x9D9A5BA1) {
    (void)mem;
    auto it = g_blocks.find((int)HLE_ARG(0));
    HLE_RET(it == g_blocks.end() ? 0 : it->second.addr);
}

PSP_HLE(SysMemUserForUser, 0x91DE343C) { (void)mem; HLE_RET(0); }   // sceKernelSetCompiledSdkVersion500_505
PSP_HLE(SysMemUserForUser, 0xF77D77CB) { (void)mem; HLE_RET(0); }   // sceKernelSetCompilerVersion

// void sceKernelPrintf(const char* fmt, ...)
PSP_HLE(SysMemUserForUser, 0x13A5ABEF) {
    psp_log(PSP_LOG_INFO, "GAME", "%s", guest_format(mem, HLE_ARG(0), ctx, 1).c_str());
    HLE_RET(0);
}

// ---------------------------------------------------------------- ModuleMgrForUser

PSP_HLE(ModuleMgrForUser, 0xD8B73127) { (void)mem; HLE_RET(MODULE_ID); }   // sceKernelGetModuleIdByAddress
PSP_HLE(ModuleMgrForUser, 0xF0A26395) { (void)mem; HLE_RET(MODULE_ID); }   // sceKernelGetModuleId

// ---------------------------------------------------------------- LoadExecForUser

// int sceKernelRegisterExitCallback(SceUID cbid)   (Callback feuert nie, kein HOME-Menü)
PSP_HLE(LoadExecForUser, 0x4AC57943) { (void)mem; g_exit_callback = (int)HLE_ARG(0); HLE_RET(0); }

// void sceKernelExitGame(void)
PSP_HLE(LoadExecForUser, 0x05572A5F) {
    (void)mem;
    psp_log(PSP_LOG_INFO, "BOOT", "sceKernelExitGame");
    HLE_RET(0);
    request_exit(0);
}

// ---------------------------------------------------------------- ThreadManForUser: Callback-Objekte
// Callbacks werden nur verwaltet, ausgelöst wird noch keiner

// SceUID sceKernelCreateCallback(const char* name, func, void* arg)
PSP_HLE(ThreadManForUser, 0xE81CAF8F) {
    const uint32_t name = HLE_ARG(0);
    const int uid = new_uid();
    g_callbacks[uid] = {name && mem_valid(name, 1) ? HLE_STR(name) : "?", HLE_ARG(1), HLE_ARG(2), cur() ? cur()->id : 0};
    HLE_RET(uid);
}

// int sceKernelDeleteCallback(SceUID cbid)
PSP_HLE(ThreadManForUser, 0xEDBA5844) {
    (void)mem;
    HLE_RET(g_callbacks.erase((int)HLE_ARG(0)) ? 0 : SCE_KERNEL_ERROR_UNKNOWN_UID);
}

// int sceKernelCheckCallback(void): 1, wenn Callbacks ausgeführt wurden
PSP_HLE(ThreadManForUser, 0x349D6D6C) { (void)mem; HLE_RET(0); }

// ---------------------------------------------------------------- scePower

// int scePowerRegisterCallback(int slot, SceUID cbid): liefert den Slot
PSP_HLE(scePower, 0x04B7766E) {
    (void)mem;
    int slot = (int)HLE_ARG(0);
    if (slot < 0) {
        for (slot = 0; slot < 16 && g_power_callbacks[slot]; slot++) {}
    }
    if (slot < 0 || slot >= 16) { HLE_RET(0x80000102u); return; }
    g_power_callbacks[slot] = (int)HLE_ARG(1);
    HLE_RET(HLE_ARG(0) == 0xFFFFFFFFu ? (uint32_t)slot : 0);
}

// int scePowerUnregisterCallback(int slot)
PSP_HLE(scePower, 0xDFA8BAF8) {
    (void)mem;
    const uint32_t slot = HLE_ARG(0);
    if (slot < 16) g_power_callbacks[slot] = 0;
    HLE_RET(0);
}

PSP_HLE(scePower, 0xEBD177D6) { (void)mem; HLE_RET(0); }   // scePowerSetClockFrequency350, Takt egal

// ---------------------------------------------------------------- sceUmdUser: Disc immer drin und lesbar

enum { UMD_PRESENT = 0x02, UMD_READY = 0x10, UMD_READABLE = 0x20 };

PSP_HLE(sceUmdUser, 0x46EBB729) { (void)mem; HLE_RET(1); }                                       // sceUmdCheckMedium
PSP_HLE(sceUmdUser, 0xC6183D47) { (void)mem; HLE_RET(0); }                                       // sceUmdActivate
PSP_HLE(sceUmdUser, 0x6B4A146C) { (void)mem; HLE_RET(UMD_PRESENT | UMD_READY | UMD_READABLE); }  // sceUmdGetDriveStat
PSP_HLE(sceUmdUser, 0x8EF08FCE) { (void)mem; HLE_RET(0); }                                       // sceUmdWaitDriveStat
PSP_HLE(sceUmdUser, 0x56202973) { (void)mem; HLE_RET(0); }                                       // sceUmdWaitDriveStatWithTimer
PSP_HLE(sceUmdUser, 0x6AF9B50A) { (void)mem; HLE_RET(0); }                                       // sceUmdCancelWaitDriveStat
PSP_HLE(sceUmdUser, 0xAEE7404D) { (void)mem; g_umd_callback = (int)HLE_ARG(0); HLE_RET(0); }     // sceUmdRegisterUMDCallBack

// ---------------------------------------------------------------- sceImpose

PSP_HLE(sceImpose, 0x36AA6E91) { (void)mem; HLE_RET(0); }   // sceImposeSetLanguageMode

// ---------------------------------------------------------------- UtilsForUser

// time_t sceKernelLibcTime(time_t* out)
PSP_HLE(UtilsForUser, 0x27CC57F0) {
    const uint32_t t = (uint32_t)time(nullptr), out = HLE_ARG(0);
    if (out && mem_valid(out, 4)) psp_w32(mem, out, t);
    HLE_RET(t);
}

// int sceKernelLibcGettimeofday(struct timeval* tv, struct timezone* tz)
PSP_HLE(UtilsForUser, 0x71EC4271) {
    const uint32_t tv = HLE_ARG(0);
    if (tv && mem_valid(tv, 8)) {
        timespec ts;
        timespec_get(&ts, TIME_UTC);
        psp_w32(mem, tv, (uint32_t)ts.tv_sec);
        psp_w32(mem, tv + 4, (uint32_t)(ts.tv_nsec / 1000));
    }
    HLE_RET(0);
}

PSP_HLE(UtilsForUser, 0x91E4F6A7) { (void)mem; HLE_RET((uint32_t)time_us()); }   // sceKernelLibcClock
PSP_HLE(UtilsForUser, 0x79D1C3FA) { (void)mem; HLE_RET(0); }                     // sceKernelDcacheWritebackAll, kein Cache
PSP_HLE(UtilsForUser, 0x3EE30821) { (void)mem; HLE_RET(0); }                     // sceKernelDcacheWritebackRange

// ---------------------------------------------------------------- StdioForUser

PSP_HLE(StdioForUser, 0x172D316E) { (void)mem; HLE_RET(0); }   // sceKernelStdin
PSP_HLE(StdioForUser, 0xA6BAB2E9) { (void)mem; HLE_RET(1); }   // sceKernelStdout
PSP_HLE(StdioForUser, 0xF78BA90A) { (void)mem; HLE_RET(2); }   // sceKernelStderr

// ---------------------------------------------------------------- sceSuspendForUser

PSP_HLE(sceSuspendForUser, 0x090CCB3F) { (void)mem; HLE_RET(0); }   // sceKernelPowerTick

// int sceKernelVolatileMemLock(int type, void** addr, int* size): 4 MiB bei 0x08400000
PSP_HLE(sceSuspendForUser, 0x3E0271D3) {
    const uint32_t paddr = HLE_ARG(1), psize = HLE_ARG(2);
    if (paddr && mem_valid(paddr, 4)) psp_w32(mem, paddr, 0x08400000u);
    if (psize && mem_valid(psize, 4)) psp_w32(mem, psize, 0x00400000u);
    HLE_RET(0);
}

PSP_HLE(sceSuspendForUser, 0xA569E425) { (void)mem; HLE_RET(0); }   // sceKernelVolatileMemUnlock

// ---------------------------------------------------------------- InterruptManager

// int sceKernelRegisterSubIntrHandler(int intr, int sub, handler, void* arg)
PSP_HLE(InterruptManager, 0xCA04A2B9) {
    (void)mem;
    psp_log(PSP_LOG_INFO, "THREAD", "sub-interrupt handler intr=%u sub=%u handler=0x%08X", HLE_ARG(0), HLE_ARG(1), HLE_ARG(2));
    HLE_RET(subintr_register((int)HLE_ARG(0), (int)HLE_ARG(1), HLE_ARG(2), HLE_ARG(3)));
}

PSP_HLE(InterruptManager, 0xFB8E22EC) { (void)mem; HLE_RET(subintr_enable((int)HLE_ARG(0), (int)HLE_ARG(1), true)); }   // sceKernelEnableSubIntr
PSP_HLE(InterruptManager, 0xD61E6961) { (void)mem; HLE_RET(subintr_release((int)HLE_ARG(0), (int)HLE_ARG(1))); }      // sceKernelReleaseSubIntrHandler

} // extern "C"
