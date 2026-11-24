// Sema, EventFlag, Mutex, LwMutex, Mbx + ein paar Thread-Funktionen (T24)
// Verhalten/Fehlercodes nach PPSSPP (Core/HLE/sceKernelSemaphore.cpp, ...EventFlag.cpp, ...Mutex.cpp, ...Mbx.cpp)
#include <algorithm>
#include <cstring>
#include <map>

#include "../src/rt.h"

using namespace rt;

namespace {

enum : uint32_t {
    ERR_UNKNOWN_THID = 0x80020198u, ERR_UNKNOWN_SEMID = 0x80020199u, ERR_UNKNOWN_EVFID = 0x8002019Au,
    ERR_UNKNOWN_MBXID = 0x8002019Bu, ERR_WAIT_TIMEOUT = 0x800201A8u, ERR_SEMA_ZERO = 0x800201ADu,
    ERR_SEMA_OVF = 0x800201AEu, ERR_EVF_COND = 0x800201AFu, ERR_EVF_ILPAT = 0x800201B1u, ERR_MBOX_NOMSG = 0x800201B2u,
    ERR_WAIT_DELETE = 0x800201B5u, ERR_ILLEGAL_COUNT = 0x800201BDu, ERR_UNKNOWN_MUTEXID = 0x800201C3u,
    ERR_MUTEX_NOT_OWNED = 0x800201C5u, ERR_MUTEX_LOCK_OVERFLOW = 0x800201C6u, ERR_MUTEX_UNLOCK_UNDERFLOW = 0x800201C7u,
    ERR_MUTEX_RECURSIVE = 0x800201C8u, ERR_ILLEGAL_ADDR = 0x800200D3u, ERR_ILLEGAL_PRIORITY = 0x80020193u
};
const uint32_t ATTR_RECURSIVE = 0x200;

struct Waiter { int thread; uint32_t a, b, c; };
struct Sema { std::string name; uint32_t attr; int init, count, max; std::vector<Waiter> waiters; };
struct EventFlag { std::string name; uint32_t attr, init, pattern; std::vector<Waiter> waiters; };
struct Mutex { std::string name; uint32_t attr; int count, owner; std::vector<Waiter> waiters; };
struct Mbx { std::string name; std::vector<uint32_t> packets; };

std::map<int, Sema> g_semas;
std::map<int, EventFlag> g_flags;
std::map<int, Mutex> g_mutexes;
std::map<int, Mbx> g_mbxs;
std::map<int, std::vector<Waiter>> g_lw_waiters;   // LwMutex-UID -> Wartende

std::string guest_name(const uint8_t* mem, uint32_t addr) {
    return addr && mem_valid(addr, 1) ? std::string((const char*)mem + addr, strnlen((const char*)mem + addr, 31)) : "?";
}

uint64_t read_timeout(const uint8_t* mem, uint32_t ptr) {
    if (!ptr || !mem_valid(ptr, 4)) return 0;
    const uint32_t us = psp_r32(mem, ptr);
    return us ? us : 1;
}

bool wake_id(int id, int result) {
    Thread* t = thread_by_id(id);
    if (!t || t->state != ThreadState::Waiting) return false;
    thread_wake(t, result);
    return true;
}

// laufenden Thread einreihen und blockieren, Ergebnis des Weckers oder Timeout
template <typename Map>
uint32_t wait_on(Map& map, int id, Waiter w, WaitType type, uint32_t timeout_ptr, uint8_t* mem) {
    Thread* t = cur();
    w.thread = t->id;
    map[id].waiters.push_back(w);
    thread_block(type, (uint32_t)id, read_timeout(mem, timeout_ptr));
    auto it = map.find(id);
    if (it == map.end()) return ERR_WAIT_DELETE;   // Objekt gelöscht
    auto& q = it->second.waiters;
    q.erase(std::remove_if(q.begin(), q.end(), [&](const Waiter& x) { return x.thread == t->id; }), q.end());
    if (t->timed_out) {
        if (timeout_ptr && mem_valid(timeout_ptr, 4)) psp_w32(mem, timeout_ptr, 0);
        return ERR_WAIT_TIMEOUT;
    }
    return (uint32_t)t->wait_result;
}

template <typename Map>
uint32_t delete_object(Map& map, int id, uint32_t unknown_error) {
    auto it = map.find(id);
    if (it == map.end()) return unknown_error;
    const std::vector<Waiter> waiters = it->second.waiters;
    map.erase(it);
    for (const Waiter& w : waiters) wake_id(w.thread, (int)ERR_WAIT_DELETE);
    return 0;
}

bool flag_matches(const EventFlag& f, uint32_t bits, uint32_t mode) {
    return (mode & 1) ? (f.pattern & bits) != 0 : (f.pattern & bits) == bits;
}

void flag_take(EventFlag& f, uint32_t bits, uint32_t mode, uint32_t out_ptr, uint8_t* mem) {
    if (out_ptr && mem_valid(out_ptr, 4)) psp_w32(mem, out_ptr, f.pattern);
    if (mode & 0x10) f.pattern = 0;
    else if (mode & 0x20) f.pattern &= ~bits;
}

uint32_t wait_event_flag(PspContext* ctx, uint8_t* mem) {
    const int id = (int)HLE_ARG(0);
    const uint32_t bits = HLE_ARG(1), mode = HLE_ARG(2), out = HLE_ARG(3), timeout = HLE_ARG(4);
    auto it = g_flags.find(id);
    if (it == g_flags.end()) return ERR_UNKNOWN_EVFID;
    if (bits == 0) return ERR_EVF_ILPAT;
    if (flag_matches(it->second, bits, mode)) {
        flag_take(it->second, bits, mode, out, mem);
        return 0;
    }
    psp_log(PSP_LOG_VERBOSE, "THREAD", "WaitEventFlag '%s' id=0x%X bits=0x%X mode=0x%X pattern=0x%X -> blocks", it->second.name.c_str(), (uint32_t)id, bits, mode, it->second.pattern);
    return wait_on(g_flags, id, {0, bits, mode, out}, WaitType::EventFlag, timeout, mem);
}

void write_name(uint8_t* mem, uint32_t addr, const std::string& name) {
    memset(mem + addr, 0, 32);
    memcpy(mem + addr, name.c_str(), std::min<size_t>(name.size(), 31));
}

} // namespace

extern "C" {

// ---------------------------------------------------------------- Semaphoren

// SceUID sceKernelCreateSema(const char* name, u32 attr, int initCount, int maxCount, option*)
PSP_HLE(ThreadManForUser, 0xD6DA4BA1) {
    const int init = (int)HLE_ARG(2), max = (int)HLE_ARG(3);
    if (init < 0 || max <= 0 || init > max) { HLE_RET(ERR_ILLEGAL_COUNT); return; }
    const int uid = new_uid();
    g_semas[uid] = {guest_name(mem, HLE_ARG(0)), HLE_ARG(1), init, init, max, {}};
    HLE_RET(uid);
}

PSP_HLE(ThreadManForUser, 0x28B6489C) { (void)mem; HLE_RET(delete_object(g_semas, (int)HLE_ARG(0), ERR_UNKNOWN_SEMID)); reschedule(); }   // sceKernelDeleteSema

// int sceKernelSignalSema(SceUID id, int count)
PSP_HLE(ThreadManForUser, 0x3F53E640) {
    (void)mem;
    auto it = g_semas.find((int)HLE_ARG(0));
    if (it == g_semas.end()) { HLE_RET(ERR_UNKNOWN_SEMID); return; }
    Sema& s = it->second;
    const int n = (int)HLE_ARG(1);
    if (s.count + n > s.max) { HLE_RET(ERR_SEMA_OVF); return; }
    s.count += n;
    for (size_t i = 0; i < s.waiters.size();) {
        const Waiter w = s.waiters[i];
        if ((int)w.a <= s.count) {
            s.count -= (int)w.a;
            s.waiters.erase(s.waiters.begin() + (ptrdiff_t)i);
            wake_id(w.thread, 0);
        } else {
            i++;
        }
    }
    HLE_RET(0);
    reschedule();
}

// int sceKernelWaitSema(SceUID id, int count, u32* timeout)
PSP_HLE(ThreadManForUser, 0x4E3A1105) {
    const int id = (int)HLE_ARG(0), need = (int)HLE_ARG(1);
    auto it = g_semas.find(id);
    if (it == g_semas.end()) { HLE_RET(ERR_UNKNOWN_SEMID); return; }
    if (need <= 0 || need > it->second.max) { HLE_RET(ERR_ILLEGAL_COUNT); return; }
    if (it->second.count >= need && it->second.waiters.empty()) {
        it->second.count -= need;
        HLE_RET(0);
        return;
    }
    HLE_RET(wait_on(g_semas, id, {0, (uint32_t)need, 0, 0}, WaitType::Sema, HLE_ARG(2), mem));
}

// int sceKernelPollSema(SceUID id, int count)
PSP_HLE(ThreadManForUser, 0x58B1F937) {
    (void)mem;
    auto it = g_semas.find((int)HLE_ARG(0));
    if (it == g_semas.end()) { HLE_RET(ERR_UNKNOWN_SEMID); return; }
    const int need = (int)HLE_ARG(1);
    if (need <= 0) { HLE_RET(ERR_ILLEGAL_COUNT); return; }
    if (it->second.count < need || !it->second.waiters.empty()) { HLE_RET(ERR_SEMA_ZERO); return; }
    it->second.count -= need;
    HLE_RET(0);
}

// int sceKernelReferSemaStatus(SceUID id, SceKernelSemaInfo* info)
PSP_HLE(ThreadManForUser, 0xBC6FEBC5) {
    auto it = g_semas.find((int)HLE_ARG(0));
    const uint32_t p = HLE_ARG(1);
    if (it == g_semas.end()) { HLE_RET(ERR_UNKNOWN_SEMID); return; }
    if (!mem_valid(p, 56)) { HLE_RET(ERR_ILLEGAL_ADDR); return; }
    const Sema& s = it->second;
    write_name(mem, p + 4, s.name);
    psp_w32(mem, p + 36, s.attr);
    psp_w32(mem, p + 40, (uint32_t)s.init);
    psp_w32(mem, p + 44, (uint32_t)s.count);
    psp_w32(mem, p + 48, (uint32_t)s.max);
    psp_w32(mem, p + 52, (uint32_t)s.waiters.size());
    HLE_RET(0);
}

// ---------------------------------------------------------------- Event-Flags

// SceUID sceKernelCreateEventFlag(const char* name, u32 attr, u32 bits, option*)
PSP_HLE(ThreadManForUser, 0x55C20A00) {
    const int uid = new_uid();
    g_flags[uid] = {guest_name(mem, HLE_ARG(0)), HLE_ARG(1), HLE_ARG(2), HLE_ARG(2), {}};
    HLE_RET(uid);
}

PSP_HLE(ThreadManForUser, 0xEF9E4C70) { (void)mem; HLE_RET(delete_object(g_flags, (int)HLE_ARG(0), ERR_UNKNOWN_EVFID)); reschedule(); }   // sceKernelDeleteEventFlag

// int sceKernelSetEventFlag(SceUID id, u32 bits)
PSP_HLE(ThreadManForUser, 0x1FB15A32) {
    auto it = g_flags.find((int)HLE_ARG(0));
    if (it == g_flags.end()) { HLE_RET(ERR_UNKNOWN_EVFID); return; }
    EventFlag& f = it->second;
    psp_log(PSP_LOG_VERBOSE, "THREAD", "SetEventFlag '%s' id=0x%X bits=0x%X (pattern 0x%X, %zu waiting)", f.name.c_str(), (uint32_t)HLE_ARG(0), HLE_ARG(1), f.pattern, f.waiters.size());
    f.pattern |= HLE_ARG(1);
    for (size_t i = 0; i < f.waiters.size();) {
        const Waiter w = f.waiters[i];
        if (flag_matches(f, w.a, w.b)) {
            flag_take(f, w.a, w.b, w.c, mem);
            f.waiters.erase(f.waiters.begin() + (ptrdiff_t)i);
            wake_id(w.thread, 0);
        } else {
            i++;
        }
    }
    HLE_RET(0);
    reschedule();
}

// int sceKernelClearEventFlag(SceUID id, u32 bits): pattern &= bits
PSP_HLE(ThreadManForUser, 0x812346E4) {
    (void)mem;
    auto it = g_flags.find((int)HLE_ARG(0));
    if (it == g_flags.end()) { HLE_RET(ERR_UNKNOWN_EVFID); return; }
    it->second.pattern &= HLE_ARG(1);
    HLE_RET(0);
}

PSP_HLE(ThreadManForUser, 0x402FCF22) { HLE_RET(wait_event_flag(ctx, mem)); }   // sceKernelWaitEventFlag
PSP_HLE(ThreadManForUser, 0x328C546A) { HLE_RET(wait_event_flag(ctx, mem)); }   // sceKernelWaitEventFlagCB

// int sceKernelPollEventFlag(SceUID id, u32 bits, u32 mode, u32* outBits)
PSP_HLE(ThreadManForUser, 0x30FD48F0) {
    auto it = g_flags.find((int)HLE_ARG(0));
    if (it == g_flags.end()) { HLE_RET(ERR_UNKNOWN_EVFID); return; }
    const uint32_t bits = HLE_ARG(1), mode = HLE_ARG(2), out = HLE_ARG(3);
    if (bits == 0) { HLE_RET(ERR_EVF_ILPAT); return; }
    if (!flag_matches(it->second, bits, mode)) {
        if (out && mem_valid(out, 4)) psp_w32(mem, out, it->second.pattern);
        HLE_RET(ERR_EVF_COND);
        return;
    }
    flag_take(it->second, bits, mode, out, mem);
    HLE_RET(0);
}

// int sceKernelReferEventFlagStatus(SceUID id, SceKernelEventFlagInfo* info)
PSP_HLE(ThreadManForUser, 0xA66B0120) {
    auto it = g_flags.find((int)HLE_ARG(0));
    const uint32_t p = HLE_ARG(1);
    if (it == g_flags.end()) { HLE_RET(ERR_UNKNOWN_EVFID); return; }
    if (!mem_valid(p, 52)) { HLE_RET(ERR_ILLEGAL_ADDR); return; }
    const EventFlag& f = it->second;
    write_name(mem, p + 4, f.name);
    psp_w32(mem, p + 36, f.attr);
    psp_w32(mem, p + 40, f.init);
    psp_w32(mem, p + 44, f.pattern);
    psp_w32(mem, p + 48, (uint32_t)f.waiters.size());
    HLE_RET(0);
}

// ---------------------------------------------------------------- Mutexe

// SceUID sceKernelCreateMutex(const char* name, u32 attr, int initCount, option*)
PSP_HLE(ThreadManForUser, 0xB7D098C6) {
    const int init = (int)HLE_ARG(2);
    if (init < 0 || (init > 1 && !(HLE_ARG(1) & ATTR_RECURSIVE))) { HLE_RET(ERR_ILLEGAL_COUNT); return; }
    const int uid = new_uid();
    g_mutexes[uid] = {guest_name(mem, HLE_ARG(0)), HLE_ARG(1), init, init ? cur()->id : 0, {}};
    HLE_RET(uid);
}

PSP_HLE(ThreadManForUser, 0xF8170FBE) { (void)mem; HLE_RET(delete_object(g_mutexes, (int)HLE_ARG(0), ERR_UNKNOWN_MUTEXID)); reschedule(); }   // sceKernelDeleteMutex

// int sceKernelLockMutex(SceUID id, int count, u32* timeout)
PSP_HLE(ThreadManForUser, 0xB011B11F) {
    const int id = (int)HLE_ARG(0), count = (int)HLE_ARG(1);
    auto it = g_mutexes.find(id);
    if (it == g_mutexes.end()) { HLE_RET(ERR_UNKNOWN_MUTEXID); return; }
    Mutex& m = it->second;
    if (count <= 0 || (count > 1 && !(m.attr & ATTR_RECURSIVE))) { HLE_RET(ERR_ILLEGAL_COUNT); return; }
    if (m.count == 0) {
        m.count = count;
        m.owner = cur()->id;
        HLE_RET(0);
        return;
    }
    if (m.owner == cur()->id) {
        if (!(m.attr & ATTR_RECURSIVE)) { HLE_RET(ERR_MUTEX_RECURSIVE); return; }
        if (m.count + count < 0) { HLE_RET(ERR_MUTEX_LOCK_OVERFLOW); return; }
        m.count += count;
        HLE_RET(0);
        return;
    }
    HLE_RET(wait_on(g_mutexes, id, {0, (uint32_t)count, 0, 0}, WaitType::Mutex, HLE_ARG(2), mem));
}

// int sceKernelUnlockMutex(SceUID id, int count)
PSP_HLE(ThreadManForUser, 0x6B30100F) {
    (void)mem;
    auto it = g_mutexes.find((int)HLE_ARG(0));
    if (it == g_mutexes.end()) { HLE_RET(ERR_UNKNOWN_MUTEXID); return; }
    Mutex& m = it->second;
    const int count = (int)HLE_ARG(1);
    if (count <= 0) { HLE_RET(ERR_ILLEGAL_COUNT); return; }
    if (m.count == 0 || m.owner != cur()->id) { HLE_RET(ERR_MUTEX_NOT_OWNED); return; }
    if (m.count < count) { HLE_RET(ERR_MUTEX_UNLOCK_UNDERFLOW); return; }
    m.count -= count;
    if (m.count == 0) {
        m.owner = 0;
        while (!m.waiters.empty()) {   // an ersten Wartenden übergeben, der noch wartet
            const Waiter w = m.waiters.front();
            m.waiters.erase(m.waiters.begin());
            if (wake_id(w.thread, 0)) {
                m.count = (int)w.a;
                m.owner = w.thread;
                break;
            }
        }
    }
    HLE_RET(0);
    reschedule();
}

// ---------------------------------------------------------------- LwMutex (Arbeitsbereich im Speicher des Spiels)
// Arbeitsbereich: { s32 lockLevel; SceUID lockThread; u32 attr; s32 numWaitThreads; SceUID uid; s32 pad[3] }

// int sceKernelCreateLwMutex(workarea*, const char* name, u32 attr, int initCount, option*)
PSP_HLE(ThreadManForUser, 0x19CFF145) {
    const uint32_t wa = HLE_ARG(0);
    const int init = (int)HLE_ARG(3);
    if (!mem_valid(wa, 32)) { HLE_RET(ERR_ILLEGAL_ADDR); return; }
    if (init < 0 || (init > 1 && !(HLE_ARG(2) & ATTR_RECURSIVE))) { HLE_RET(ERR_ILLEGAL_COUNT); return; }
    const int uid = new_uid();
    g_lw_waiters[uid];
    memset(mem + wa, 0, 32);
    psp_w32(mem, wa, (uint32_t)init);
    psp_w32(mem, wa + 4, init ? (uint32_t)cur()->id : 0);
    psp_w32(mem, wa + 8, HLE_ARG(2));
    psp_w32(mem, wa + 16, (uint32_t)uid);
    HLE_RET(0);
}

// int sceKernelDeleteLwMutex(workarea*)
PSP_HLE(ThreadManForUser, 0x60107536) {
    const uint32_t wa = HLE_ARG(0);
    if (!mem_valid(wa, 32)) { HLE_RET(ERR_ILLEGAL_ADDR); return; }
    const int uid = (int)psp_r32(mem, wa + 16);
    auto it = g_lw_waiters.find(uid);
    if (it == g_lw_waiters.end()) { HLE_RET(ERR_UNKNOWN_MUTEXID); return; }
    const std::vector<Waiter> waiters = it->second;
    g_lw_waiters.erase(it);
    psp_w32(mem, wa + 16, 0xFFFFFFFFu);
    for (const Waiter& w : waiters) wake_id(w.thread, (int)ERR_WAIT_DELETE);
    HLE_RET(0);
    reschedule();
}

// int sceKernelLockLwMutexCB(workarea*, int count, u32* timeout)
PSP_HLE(Kernel_Library, 0x1FC64E09) {
    const uint32_t wa = HLE_ARG(0);
    const int count = (int)HLE_ARG(1);
    if (!mem_valid(wa, 32)) { HLE_RET(ERR_ILLEGAL_ADDR); return; }
    const int uid = (int)psp_r32(mem, wa + 16);
    if (!g_lw_waiters.count(uid)) { HLE_RET(ERR_UNKNOWN_MUTEXID); return; }
    const int level = (int)psp_r32(mem, wa), owner = (int)psp_r32(mem, wa + 4);
    const uint32_t attr = psp_r32(mem, wa + 8);
    if (count <= 0 || (count > 1 && !(attr & ATTR_RECURSIVE))) { HLE_RET(ERR_ILLEGAL_COUNT); return; }
    if (level == 0) {
        psp_w32(mem, wa, (uint32_t)count);
        psp_w32(mem, wa + 4, (uint32_t)cur()->id);
        HLE_RET(0);
        return;
    }
    if (owner == cur()->id) {
        if (!(attr & ATTR_RECURSIVE)) { HLE_RET(ERR_MUTEX_RECURSIVE); return; }
        psp_w32(mem, wa, (uint32_t)(level + count));
        HLE_RET(0);
        return;
    }
    Thread* t = cur();
    g_lw_waiters[uid].push_back({t->id, (uint32_t)count, 0, 0});
    psp_w32(mem, wa + 12, (uint32_t)g_lw_waiters[uid].size());
    thread_block(WaitType::Mutex, (uint32_t)uid, read_timeout(mem, HLE_ARG(2)));
    auto it = g_lw_waiters.find(uid);
    if (it == g_lw_waiters.end()) { HLE_RET(ERR_WAIT_DELETE); return; }
    auto& q = it->second;
    q.erase(std::remove_if(q.begin(), q.end(), [&](const Waiter& x) { return x.thread == t->id; }), q.end());
    psp_w32(mem, wa + 12, (uint32_t)q.size());
    HLE_RET(t->timed_out ? ERR_WAIT_TIMEOUT : (uint32_t)t->wait_result);
}

// int sceKernelUnlockLwMutex(workarea*, int count)
PSP_HLE(Kernel_Library, 0x15B6446B) {
    const uint32_t wa = HLE_ARG(0);
    const int count = (int)HLE_ARG(1);
    if (!mem_valid(wa, 32)) { HLE_RET(ERR_ILLEGAL_ADDR); return; }
    const int uid = (int)psp_r32(mem, wa + 16);
    auto it = g_lw_waiters.find(uid);
    if (it == g_lw_waiters.end()) { HLE_RET(ERR_UNKNOWN_MUTEXID); return; }
    int level = (int)psp_r32(mem, wa);
    if (count <= 0) { HLE_RET(ERR_ILLEGAL_COUNT); return; }
    if (level == 0 || (int)psp_r32(mem, wa + 4) != cur()->id) { HLE_RET(ERR_MUTEX_NOT_OWNED); return; }
    if (level < count) { HLE_RET(ERR_MUTEX_UNLOCK_UNDERFLOW); return; }
    level -= count;
    psp_w32(mem, wa, (uint32_t)level);
    if (level == 0) {
        psp_w32(mem, wa + 4, 0);
        auto& q = it->second;
        while (!q.empty()) {
            const Waiter w = q.front();
            q.erase(q.begin());
            if (wake_id(w.thread, 0)) {
                psp_w32(mem, wa, w.a);
                psp_w32(mem, wa + 4, (uint32_t)w.thread);
                break;
            }
        }
        psp_w32(mem, wa + 12, (uint32_t)q.size());
    }
    HLE_RET(0);
    reschedule();
}

// ---------------------------------------------------------------- Mailboxen (nur Send und Poll werden importiert)

PSP_HLE(ThreadManForUser, 0x8125221D) {   // sceKernelCreateMbx(name, attr, option*)
    const int uid = new_uid();
    g_mbxs[uid] = {guest_name(mem, HLE_ARG(0)), {}};
    HLE_RET(uid);
}

PSP_HLE(ThreadManForUser, 0x86255ADA) { (void)mem; HLE_RET(g_mbxs.erase((int)HLE_ARG(0)) ? 0 : ERR_UNKNOWN_MBXID); }   // sceKernelDeleteMbx

PSP_HLE(ThreadManForUser, 0xE9B3061E) {   // sceKernelSendMbx(id, void* packet)
    (void)mem;
    auto it = g_mbxs.find((int)HLE_ARG(0));
    if (it == g_mbxs.end()) { HLE_RET(ERR_UNKNOWN_MBXID); return; }
    it->second.packets.push_back(HLE_ARG(1));
    HLE_RET(0);
}

PSP_HLE(ThreadManForUser, 0x0D81716A) {   // sceKernelPollMbx(id, void** packet)
    auto it = g_mbxs.find((int)HLE_ARG(0));
    if (it == g_mbxs.end()) { HLE_RET(ERR_UNKNOWN_MBXID); return; }
    if (it->second.packets.empty()) { HLE_RET(ERR_MBOX_NOMSG); return; }
    const uint32_t out = HLE_ARG(1);
    if (out && mem_valid(out, 4)) psp_w32(mem, out, it->second.packets.front());
    it->second.packets.erase(it->second.packets.begin());
    HLE_RET(0);
}

// ---------------------------------------------------------------- Threads: Attribute, Priorität, Status

PSP_HLE(ThreadManForUser, 0xEA748E31) { (void)mem; HLE_RET(0); }   // sceKernelChangeCurrentThreadAttr (VFPU/FPU immer da)

// int sceKernelChangeThreadPriority(SceUID id, int priority)   (id 0 = eigener Thread)
PSP_HLE(ThreadManForUser, 0x71BC9871) {
    (void)mem;
    Thread* t = HLE_ARG(0) ? thread_by_id((int)HLE_ARG(0)) : cur();
    const int prio = (int)HLE_ARG(1);
    if (!t) { HLE_RET(ERR_UNKNOWN_THID); return; }
    if (prio < 0x08 || prio > 0x77) { HLE_RET(ERR_ILLEGAL_PRIORITY); return; }
    t->priority = prio;
    HLE_RET(0);
    thread_yield();   // neu wählen, auch wenn die eigene Prio gesunken ist
}

// int sceKernelTerminateThread(SceUID id) / sceKernelTerminateDeleteThread(SceUID id)
PSP_HLE(ThreadManForUser, 0x616403BA) { (void)mem; HLE_RET(thread_terminate((int)HLE_ARG(0), false)); }
PSP_HLE(ThreadManForUser, 0x383F7BCC) { (void)mem; HLE_RET(thread_terminate((int)HLE_ARG(0), true)); }

// int sceKernelWakeupThread(SceUID id): weckt einen Thread aus sceKernelSleepThread
PSP_HLE(ThreadManForUser, 0xD59EAD2F) {
    (void)mem;
    Thread* t = thread_by_id((int)HLE_ARG(0));
    if (!t) { HLE_RET(ERR_UNKNOWN_THID); return; }
    if (t->state == ThreadState::Waiting && t->wait == WaitType::Sleep) thread_wake(t, 0);
    HLE_RET(0);
    reschedule();
}

// int sceKernelReferThreadStatus(SceUID id, SceKernelThreadInfo* info)
PSP_HLE(ThreadManForUser, 0x17C1684E) {
    Thread* t = HLE_ARG(0) ? thread_by_id((int)HLE_ARG(0)) : cur();
    const uint32_t p = HLE_ARG(1);
    if (!t) { HLE_RET(ERR_UNKNOWN_THID); return; }
    if (!mem_valid(p, 104)) { HLE_RET(ERR_ILLEGAL_ADDR); return; }
    const uint32_t size = psp_r32(mem, p);
    // ExitThread-Thread (bei uns Dead) = DORMANT (0x10), nicht KILLED (0x20); Sofdec wartet darauf (soundThread, 4 -> 5)
    static const uint32_t status[] = {16 /*dormant*/, 2 /*ready*/, 1 /*running*/, 4 /*waiting*/, 16 /*dead = dormant*/};
    write_name(mem, p + 4, t->name);
    psp_w32(mem, p + 36, t->attr);
    psp_w32(mem, p + 40, status[(int)t->state]);
    psp_w32(mem, p + 44, t->entry);
    psp_w32(mem, p + 48, t->stack_addr);
    psp_w32(mem, p + 52, t->stack_size);
    psp_w32(mem, p + 56, t->ctx.r[PSP_GP]);
    psp_w32(mem, p + 60, (uint32_t)t->init_priority);
    psp_w32(mem, p + 64, (uint32_t)t->priority);
    psp_w32(mem, p + 68, (uint32_t)t->wait);
    psp_w32(mem, p + 72, t->wait_object);
    psp_w32(mem, p + 76, 0);
    psp_w32(mem, p + 80, (uint32_t)t->exit_status);
    if (size >= 104) memset(mem + p + 84, 0, 20);
    HLE_RET(0);
}

} // extern "C"
