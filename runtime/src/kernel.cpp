// PSP-Threads als Fibers auf einem Host-Thread, Scheduler, Zeit, VBlank.
// Kooperativ: es läuft der bereite Thread mit kleinster Prioritätszahl, Wechsel nur in Syscalls/Safepoints.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <memory>

#include "psp_vfpu.h"
#include "rt.h"

namespace rt {

namespace {

const uint64_t VBLANK_PERIOD_US = 16683;          // 59,94 Hz
const int32_t SAFEPOINT_BUDGET = 20000;
const uint32_t SCE_KERNEL_ERROR_NO_MEMORY = 0x80020190u;
const uint32_t SCE_KERNEL_ERROR_UNKNOWN_THID = 0x80020198u;
const uint32_t SCE_KERNEL_ERROR_NOT_DORMANT = 0x800201A4u;
const uint32_t SCE_KERNEL_ERROR_ILLEGAL_PRIORITY = 0x80020193u;
const uint32_t SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE = 0x80020192u;

std::vector<std::unique_ptr<Thread>> g_threads;
Thread* g_cur = nullptr;
void* g_sched_fiber = nullptr;
uint64_t g_seq = 1;
int g_next_id = 0x100;
uint32_t g_gp = 0;
LARGE_INTEGER g_freq, g_start;
uint64_t g_next_vblank = VBLANK_PERIOD_US;
uint32_t g_vblank_count = 0;
int g_intr_disabled = 0;
bool g_exit_requested = false;
int g_exit_code = 0;
std::vector<Thread*> g_dead_fibers;   // Fibers, die der Scheduler noch löschen muss

// Sub-Interrupts: laufen auf einem eigenen Fiber mit eigenem Stack, zwischen zwei Threadwechseln
const int INTR_VBLANK = 30, INTR_MAX = 67, SUBINTR_MAX = 32;
struct SubIntr { uint32_t handler = 0, arg = 0; bool enabled = false; };
SubIntr g_subintr[INTR_MAX][SUBINTR_MAX];
bool g_vblank_intr_pending = false;
Thread g_intr_thread;

void to_scheduler() { SwitchToFiber(g_sched_fiber); }

void make_ready(Thread* t) {
    t->state = ThreadState::Ready;
    t->wait = WaitType::None;
    t->wake_time = 0;
    t->ready_seq = g_seq++;
}

VOID CALLBACK fiber_proc(void* param) {
    Thread* t = (Thread*)param;
    psp_call_indirect(&t->ctx, g_mem, t->entry);
    thread_exit((int)t->ctx.r[PSP_V0], false);   // Rückkehr aus der Einstiegsfunktion = sceKernelExitThread(v0)
}

// Früheste Zeit, zu der etwas passiert (VBlank oder Ablauf einer Wartezeit)
uint64_t next_event_time() {
    uint64_t t = g_next_vblank;
    for (const auto& th : g_threads)
        if (th->state == ThreadState::Waiting && th->wake_time && th->wake_time < t) t = th->wake_time;
    return t;
}

void process_time(uint64_t now) {
    while (now >= g_next_vblank) {
        g_vblank_count++;
        g_vblank_intr_pending = true;
        g_next_vblank += VBLANK_PERIOD_US;
        if (now > g_next_vblank + 10 * VBLANK_PERIOD_US) g_next_vblank = now + VBLANK_PERIOD_US;   // nach langer Pause nicht nachholen
        for (const auto& th : g_threads)
            if (th->state == ThreadState::Waiting && th->wait == WaitType::Vblank) {
                th->wait_result = 0;
                make_ready(th.get());
            }
    }
    for (const auto& th : g_threads)
        if (th->state == ThreadState::Waiting && th->wake_time && now >= th->wake_time) {
            th->timed_out = th->wait != WaitType::Delay;
            th->wait_result = 0;
            make_ready(th.get());
        }
}

Thread* pick_ready() {
    Thread* best = nullptr;
    for (const auto& th : g_threads) {
        if (th->state != ThreadState::Ready) continue;
        if (!best || th->priority < best->priority || (th->priority == best->priority && th->ready_seq < best->ready_seq))
            best = th.get();
    }
    return best;
}

struct IntrCall { uint32_t func, a0, a1, a2; };
std::vector<IntrCall> g_intr_calls;

VOID CALLBACK intr_fiber_proc(void*) {
    for (;;) {
        while (!g_intr_calls.empty()) {
            const IntrCall call = g_intr_calls.front();
            g_intr_calls.erase(g_intr_calls.begin());
            PspContext& c = g_intr_thread.ctx;
            c.r[PSP_SP] = g_intr_thread.stack_addr + g_intr_thread.stack_size - 0x40;
            c.r[PSP_GP] = g_gp;
            c.r[PSP_A0] = call.a0;
            c.r[PSP_A1] = call.a1;
            c.r[PSP_A2] = call.a2;
            c.r[PSP_RA] = 0x08000000u;
            c.budget = 0x7FFFFFFF;
            psp_call_indirect(&c, g_mem, call.func);
        }
        to_scheduler();
    }
}

void run_interrupts() {
    if (!g_intr_thread.fiber) {
        g_intr_thread.name = "intr";
        g_intr_thread.id = 1;
        g_intr_thread.stack_size = 0x4000;
        g_intr_thread.stack_addr = mem_alloc(g_intr_thread.stack_size, true, "interrupt stack");
        psp_vfpu_reset(&g_intr_thread.ctx);
        g_intr_thread.ctx.fcr31 = 0x00000E00u;
        g_intr_thread.fiber = CreateFiberEx(0x10000, g_opt.host_stack_size, FIBER_FLAG_FLOAT_SWITCH, intr_fiber_proc, nullptr);
        if (!g_intr_thread.fiber || !g_intr_thread.stack_addr) fatal(EXIT_FATAL, "THREAD", "Interrupt-Fiber konnte nicht angelegt werden");
    }
    g_intr_disabled = 1;          // im Handler: keine Safepoint-Wechsel
    g_cur = &g_intr_thread;
    g_intr_thread.state = ThreadState::Running;
    SwitchToFiber(g_intr_thread.fiber);
    g_cur = nullptr;
    g_intr_disabled = 0;
}

uint64_t g_idle_us = 0;
} // namespace
uint64_t g_perf_ge_us = 0;
namespace {   // Zeit im Leerlauf (Statistik PERF)

void sleep_until(uint64_t target) {
    const uint64_t t0 = time_us();
    struct Acc { uint64_t t0; ~Acc() { g_idle_us += time_us() - t0; } } acc{t0};
    for (;;) {
        const uint64_t now = time_us();
        if (now >= target) return;
        const uint64_t left = target - now;
        if (left > 2000) Sleep((DWORD)((left - 1000) / 1000));
        else SwitchToThread();
    }
}

} // namespace

uint64_t time_us() {
    if (!g_freq.QuadPart) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_start);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart - g_start.QuadPart) * 1000000 / g_freq.QuadPart);
}

void kernel_init(uint32_t gp) {
    g_gp = gp;
    time_us();
    timeBeginPeriod(1);
    g_sched_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!g_sched_fiber) fatal(EXIT_FATAL, "THREAD", "ConvertThreadToFiber fehlgeschlagen (%lu)", GetLastError());
}

Thread* cur() { return g_cur; }

Thread* thread_by_id(int id) {
    for (const auto& th : g_threads)
        if (th->id == id) return th.get();
    return nullptr;
}

int thread_create(const char* name, uint32_t entry, int priority, uint32_t stack_size, uint32_t attr) {
    if (priority < 0x08 || priority > 0x77) return (int)SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;
    if (stack_size < 0x200) return (int)SCE_KERNEL_ERROR_ILLEGAL_STACK_SIZE;
    stack_size = (stack_size + 0xFF) & ~0xFFu;
    auto t = std::make_unique<Thread>();
    t->stack_addr = mem_alloc(stack_size, true, name);
    if (!t->stack_addr) {
        psp_log(PSP_LOG_ERROR, "THREAD", "no memory for stack of '%s' (0x%X bytes)", name, stack_size);
        return (int)SCE_KERNEL_ERROR_NO_MEMORY;
    }
    t->id = g_next_id++;
    t->name = name ? name : "?";
    for (char& c : t->name)
        if (c == '|') c = '/';
    t->entry = entry;
    t->attr = attr;
    t->init_priority = t->priority = priority;
    t->stack_size = stack_size;
    memset(g_mem + t->stack_addr, 0xFF, stack_size);
    psp_w32(g_mem, t->stack_addr, (uint32_t)t->id);
    psp_log(PSP_LOG_INFO, "THREAD", "create '%s' id=0x%X entry=0x%08X prio=%d stack=0x%08X+0x%X", t->name.c_str(), t->id, entry,
            priority, t->stack_addr, stack_size);
    g_threads.push_back(std::move(t));
    return g_threads.back()->id;
}

int thread_start(int id, uint32_t arg_len, uint32_t arg_ptr) {
    Thread* t = thread_by_id(id);
    if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
    if (t->state != ThreadState::Dormant) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;

    // Startkontext wie auf der PSP (nach PPSSPP: __KernelResetThread/__KernelStartThread)
    PspContext& c = t->ctx;
    memset(&c, 0, sizeof c);
    psp_vfpu_reset(&c);
    c.fcr31 = 0x00000E00u;
    c.budget = SAFEPOINT_BUDGET;
    c.thread = t;
    uint32_t sp = t->stack_addr + t->stack_size - 0x100;
    const uint32_t k0 = sp;
    memset(g_mem + k0, 0, 0x100);
    psp_w32(g_mem, k0 + 0xC0, (uint32_t)t->id);
    psp_w32(g_mem, k0 + 0xC8, t->stack_addr);
    psp_w32(g_mem, k0 + 0xF8, 0xFFFFFFFFu);
    psp_w32(g_mem, k0 + 0xFC, 0xFFFFFFFFu);
    if (arg_len && arg_ptr) {
        sp -= (arg_len + 0xF) & ~0xFu;
        memmove(g_mem + sp, g_mem + arg_ptr, arg_len);
        c.r[PSP_A0] = arg_len;
        c.r[PSP_A1] = sp;
    }
    sp -= 64;
    c.r[PSP_SP] = sp;
    c.r[PSP_FP] = sp;
    c.r[PSP_K0] = k0;
    c.r[PSP_GP] = g_gp;
    c.r[PSP_RA] = 0x08000000u;   // nie angesprungen, Rückkehr geht über C-Return
    c.pc = t->entry;
    t->priority = t->init_priority;
    t->exit_status = 0;

    if (t->fiber) DeleteFiber(t->fiber);
    t->fiber = CreateFiberEx(0x10000, g_opt.host_stack_size, FIBER_FLAG_FLOAT_SWITCH, fiber_proc, t);
    if (!t->fiber) fatal(EXIT_FATAL, "THREAD", "CreateFiber fehlgeschlagen (%lu)", GetLastError());
    make_ready(t);
    psp_log(PSP_LOG_INFO, "THREAD", "start '%s' id=0x%X", t->name.c_str(), t->id);
    reschedule();
    return 0;
}

void thread_exit(int status, bool delete_self) {
    Thread* t = g_cur;
    t->exit_status = status;
    t->state = ThreadState::Dead;
    t->delete_on_exit = delete_self;
    psp_log(PSP_LOG_INFO, "THREAD", "exit '%s' id=0x%X status=0x%X", t->name.c_str(), t->id, status);
    for (const auto& th : g_threads)
        if (th->state == ThreadState::Waiting && th->wait == WaitType::ThreadEnd && th->wait_object == (uint32_t)t->id) {
            th->wait_result = 0;
            make_ready(th.get());
        }
    g_dead_fibers.push_back(t);
    to_scheduler();   // kehrt nicht zurück
    fatal(EXIT_FATAL, "THREAD", "beendeter Thread wurde fortgesetzt");
}

int thread_delete(int id) {
    Thread* t = thread_by_id(id);
    if (!t) return (int)SCE_KERNEL_ERROR_UNKNOWN_THID;
    if (t->state != ThreadState::Dormant && t->state != ThreadState::Dead) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
    if (t == g_cur) return (int)SCE_KERNEL_ERROR_NOT_DORMANT;
    if (std::find(g_dead_fibers.begin(), g_dead_fibers.end(), t) != g_dead_fibers.end()) {
        t->delete_on_exit = true;   // Fiber läuft formal noch, Scheduler räumt auf
        return 0;
    }
    if (t->fiber) DeleteFiber(t->fiber);
    mem_free(t->stack_addr);
    g_threads.erase(std::find_if(g_threads.begin(), g_threads.end(), [&](const auto& p) { return p.get() == t; }));
    return 0;
}

void thread_block(WaitType type, uint32_t object, uint64_t timeout_us) {
    Thread* t = g_cur;
    if (!t) fatal(EXIT_FATAL, "THREAD", "Warten außerhalb eines PSP-Threads");
    t->state = ThreadState::Waiting;
    t->wait = type;
    t->wait_object = object;
    t->timed_out = false;
    t->wait_result = 0;
    t->wake_time = timeout_us ? time_us() + timeout_us : 0;
    to_scheduler();
}

void thread_wake(Thread* t, int result) {
    if (t->state != ThreadState::Waiting) return;
    t->wait_result = result;
    make_ready(t);
}

void reschedule() {
    Thread* t = g_cur;
    if (!t || g_intr_disabled) return;
    Thread* best = pick_ready();
    if (!best || best->priority >= t->priority) return;   // nur ein Thread höherer Priorität verdrängt
    t->state = ThreadState::Ready;                        // ready_seq bleibt: der Thread behält seinen Platz
    to_scheduler();
}

void thread_yield() {
    Thread* t = g_cur;
    if (!t || g_intr_disabled) return;
    t->state = ThreadState::Ready;
    to_scheduler();
}

int thread_terminate(int id, bool delete_thread) {
    Thread* t = thread_by_id(id);
    if (!t || t == g_cur) return (int)0x80020198u;   // UNKNOWN_THID (sich selbst beenden geht über ExitThread)
    if (t->state != ThreadState::Dormant && t->state != ThreadState::Dead) {
        psp_log(PSP_LOG_INFO, "THREAD", "terminate '%s' id=0x%X", t->name.c_str(), t->id);
        t->state = ThreadState::Dead;
        t->exit_status = (int)0x800201ACu;            // THREAD_TERMINATED
        for (const auto& th : g_threads)
            if (th->state == ThreadState::Waiting && th->wait == WaitType::ThreadEnd && th->wait_object == (uint32_t)t->id) {
                th->wait_result = 0;
                make_ready(th.get());
            }
        if (t->fiber) {
            DeleteFiber(t->fiber);
            t->fiber = nullptr;
        }
    }
    return delete_thread ? thread_delete(id) : 0;
}

void intr_queue_call(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2) { g_intr_calls.push_back({func, a0, a1, a2}); }

void wait_vblank() { thread_block(WaitType::Vblank, 0, 0); }
uint32_t vblank_count() { return g_vblank_count; }

int intr_suspend() {
    const int old = g_intr_disabled;
    g_intr_disabled = 1;
    return old;
}

void intr_resume(int old_state) {
    g_intr_disabled = old_state ? 1 : 0;
}

void request_exit(int code) {
    g_exit_requested = true;
    g_exit_code = code;
    if (g_cur) {
        g_cur->state = ThreadState::Ready;
        to_scheduler();
    }
}

int exit_code() { return g_exit_code; }

int new_uid() { return g_next_id++; }

int subintr_register(int intr, int sub, uint32_t handler, uint32_t arg) {
    if (intr < 0 || intr >= INTR_MAX || sub < 0 || sub >= SUBINTR_MAX) return (int)0x80020065u;   // ILLEGAL_INTRCODE
    g_subintr[intr][sub] = {handler, arg, false};
    return 0;
}

int subintr_enable(int intr, int sub, bool enable) {
    if (intr < 0 || intr >= INTR_MAX || sub < 0 || sub >= SUBINTR_MAX) return (int)0x80020065u;
    g_subintr[intr][sub].enabled = enable;
    return 0;
}

int subintr_release(int intr, int sub) {
    if (intr < 0 || intr >= INTR_MAX || sub < 0 || sub >= SUBINTR_MAX) return (int)0x80020065u;
    g_subintr[intr][sub] = SubIntr{};
    return 0;
}

void dump_threads() {
    static const char* const states[] = {"dormant", "ready", "running", "waiting", "dead"};
    static const char* const waits[] = {"-", "delay", "sleep", "thread_end", "vblank", "sema", "event_flag", "mutex", "other"};
    for (const auto& th : g_threads) {
        if (th->state == ThreadState::Dead) continue;
        psp_log(PSP_LOG_INFO, "THREAD", "DUMP '%s' id=0x%X prio=%d %s wait=%s obj=0x%X wake=%llu ra=0x%08X", th->name.c_str(), th->id, th->priority,
                states[(int)th->state], waits[(int)th->wait], th->wait_object, (unsigned long long)th->wake_time, th->ctx.r[PSP_RA]);
        const uint32_t s0 = th->ctx.r[16];   // fürs Debuggen von Zustandsmaschinen: s0-s2 und Wort bei s0+4
        psp_log(PSP_LOG_INFO, "THREAD", "DUMP   regs s0=0x%08X s1=0x%08X s2=0x%08X [s0+4]=0x%08X [s0]=0x%08X", s0, th->ctx.r[17], th->ctx.r[18],
                mem_valid(s0 + 4, 4) ? psp_r32(g_mem, s0 + 4) : 0, mem_valid(s0, 4) ? psp_r32(g_mem, s0) : 0);
    }
}

static void check_watch(const Thread* last) {
    static std::vector<uint32_t> old;
    if (old.size() != g_opt.watch.size()) old.assign(g_opt.watch.size(), 0);
    for (size_t i = 0; i < g_opt.watch.size(); i++) {
        const uint32_t a = g_opt.watch[i];
        if (!mem_valid(a, 4)) continue;
        const uint32_t v = psp_r32(g_mem, a);
        if (v != old[i]) {
            psp_log(PSP_LOG_INFO, "THREAD", "WATCH 0x%08X: 0x%08X -> 0x%08X (zuletzt lief '%s', ra=0x%08X)", a, old[i], v, last ? last->name.c_str() : "-", last ? last->ctx.r[PSP_RA] : 0);
            old[i] = v;
        }
    }
}

void scheduler_run() {
    uint64_t next_dump = 0;
    for (;;) {
        // beendete Threads aufräumen
        for (Thread* d : g_dead_fibers) {
            if (d->fiber) {
                DeleteFiber(d->fiber);
                d->fiber = nullptr;
            }
            if (d->delete_on_exit) {
                mem_free(d->stack_addr);
                g_threads.erase(std::find_if(g_threads.begin(), g_threads.end(), [&](const auto& p) { return p.get() == d; }));
            }
        }
        g_dead_fibers.clear();
        if (g_exit_requested) return;

        process_time(time_us());
        {   // PERF alle 10 s: Anteil Leerlauf und GE-Zeichnen, VBlanks
            static uint64_t next = 10000000, idle0 = 0, ge0 = 0;
            static uint32_t vb0 = 0;
            const uint64_t now = time_us();
            if (now >= next) {
                psp_log(PSP_LOG_INFO, "PERF", "Leerlauf %.0f%%, GE %.0f%%, VBlanks %u", (g_idle_us - idle0) / 100000.0,
                        (g_perf_ge_us - ge0) / 100000.0, g_vblank_count - vb0);
                idle0 = g_idle_us; ge0 = g_perf_ge_us; vb0 = g_vblank_count; next = now + 10000000;
            }
        }
        {
            static uint32_t shown = 0;
            if (shown != g_vblank_count) { shown = g_vblank_count; window_vblank(g_mem); }
        }
        if (g_opt.dump_threads_sec > 0 && time_us() >= next_dump) {
            if (next_dump) dump_threads();
            next_dump = time_us() + (uint64_t)g_opt.dump_threads_sec * 1000000ull;
        }
        if (!g_intr_disabled) {
            if (g_vblank_intr_pending) {
                g_vblank_intr_pending = false;
                for (int sub = 0; sub < SUBINTR_MAX; sub++) {
                    const SubIntr& h = g_subintr[INTR_VBLANK][sub];
                    if (h.enabled && h.handler) g_intr_calls.push_back({h.handler, (uint32_t)sub, h.arg, 0});
                }
            }
            if (!g_intr_calls.empty()) run_interrupts();
        }
        if (Thread* t = pick_ready()) {
            g_cur = t;
            t->state = ThreadState::Running;
            SwitchToFiber(t->fiber);
            g_cur = nullptr;
            if (!g_opt.watch.empty()) check_watch(t);
            continue;
        }
        bool alive = false;
        for (const auto& th : g_threads) alive |= th->state == ThreadState::Waiting;
        if (!alive) return;   // alle Threads beendet oder nie gestartet
        sleep_until(next_event_time());
    }
}

} // namespace rt

extern "C" void psp_safepoint(PspContext* ctx, uint8_t* mem) {
    (void)mem;
    ctx->budget = rt::SAFEPOINT_BUDGET;
    rt::Thread* t = rt::g_cur;
    if (!t || rt::g_intr_disabled) return;
    if (rt::time_us() < rt::next_event_time()) return;
    // Zeitereignis fällig: Scheduler stellt zu und wählt neu
    t->state = rt::ThreadState::Ready;
    rt::to_scheduler();
}
