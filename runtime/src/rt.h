// interne Runtime-Schnittstellen; nach außen: psp_context.h, psp_hle.h, psp_vfpu.h
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "psp_context.h"
#include "psp_hle.h"

namespace rt {

// Exit-Codes der .exe
enum {
    EXIT_OK = 0, EXIT_USAGE = 2, EXIT_NOT_FOUND = 10, EXIT_WRONG_HASH = 12, EXIT_ENCRYPTED = 13,
    EXIT_DEVICE = 60, EXIT_IPC = 61, EXIT_UNIMPL = 70, EXIT_MEMORY = 71, EXIT_BREAK = 72, EXIT_TARGET = 73,
    EXIT_FATAL = 74, EXIT_CXX = 75
};

struct Options {
    std::string iso, eboot, log_path, save_dir, ipc;
    PspLogLevel log_level = PSP_LOG_INFO;
    int scale = 1;
    bool window = false;
    bool mute = false;        // --mute: kein Ton
    bool overlay = false;     // --overlay: Leistungsanzeige gleich beim Start an (sonst F3)
    std::string settings_path;       // --settings <datei>
    std::string lang = "de";         // settings.ini lang=de|en
    std::string alpha_test;          // --alpha-test expired|tamper
    bool alpha_tamper = false;       // Fall B
    bool input_log = false;   // --input-log: Latenz Taste -> Spiel -> Bild -> Treffer-Ton ins Log (INPUT)
    std::vector<int> shots;   // --shot <sekunden>, mehrfach
    std::vector<std::pair<uint64_t, uint32_t>> presses;   // --press <s>:<taste>: Zeitpunkt (µs), Tastenbits
    std::vector<uint32_t> watch;             // --watch <adresse>: Änderungen des Wortes im Log melden (Fehlersuche)
    int dump_threads_sec = 0;                // >0: alle n s Threadzustand ins Log (bei Hängern)
    uint32_t user_ram_size = 0x01800000;     // 24 MiB ab 0x08800000
    uint32_t host_stack_size = 0x00100000;   // 1 MiB je PSP-Thread
};
extern Options g_opt;

// ---------------------------------------------------------------- Log (log.cpp)
void log_init(const std::string& path, PspLogLevel level);
void log_flush();                                      // Log auf die Platte (Wächter, jede Sekunde)
void log_shutdown();                                   // schreibt die UNIMPL-COUNT-Zeilen
[[noreturn]] void fatal(int exit_code, const char* category, const char* fmt, ...);
[[noreturn]] void exit_process(int exit_code);         // Log abschließen und beenden

// ---------------------------------------------------------------- Speicher (memory.cpp)
extern uint8_t* g_mem;                                  // Anfang der 4-GiB-Reservierung
void mem_init();
bool mem_valid(uint32_t addr, uint32_t size);           // liegt [addr, addr+size) in einem eingeblendeten Bereich?
void mem_set_user_range(uint32_t lo, uint32_t hi);      // freier User-Speicher für mem_alloc
uint32_t mem_alloc(uint32_t size, bool high, const char* name);   // 0 = kein Platz; auf 256 Bytes gerundet
bool mem_free(uint32_t addr);
uint32_t mem_free_total();
uint32_t mem_free_max();

// ---------------------------------------------------------------- Kernel (kernel.cpp)
enum class ThreadState { Dormant, Ready, Running, Waiting, Dead };
enum class WaitType { None, Delay, Sleep, ThreadEnd, Vblank, Sema, EventFlag, Mutex, Other };

struct Thread {
    int id = 0;
    std::string name;
    PspContext ctx{};
    void* fiber = nullptr;
    ThreadState state = ThreadState::Dormant;
    WaitType wait = WaitType::None;
    uint32_t wait_object = 0;      // z.B. Thread-ID bei ThreadEnd
    uint64_t wake_time = 0;        // 0 = kein Zeitlimit; sonst Zeitpunkt in µs
    int wait_result = 0;           // vom Wecker gesetzt (0 = Erfolg, sonst PSP-Fehlercode)
    bool timed_out = false;
    uint32_t entry = 0, attr = 0;
    int init_priority = 32, priority = 32;
    uint32_t stack_addr = 0, stack_size = 0;
    uint64_t ready_seq = 0;        // Reihenfolge innerhalb gleicher Priorität
    int exit_status = 0;
    bool delete_on_exit = false;
};

uint64_t time_us();                                     // Mikrosekunden seit Start
void kernel_init(uint32_t gp);
Thread* cur();                                          // laufender PSP-Thread oder nullptr
Thread* thread_by_id(int id);
int  thread_create(const char* name, uint32_t entry, int priority, uint32_t stack_size, uint32_t attr);   // ID oder Fehlercode < 0
int  thread_start(int id, uint32_t arg_len, uint32_t arg_ptr);
void thread_exit(int status, bool delete_self);         // kehrt nicht zurück
int  thread_delete(int id);
void thread_block(WaitType type, uint32_t object, uint64_t timeout_us);   // blockiert den laufenden Thread; 0 = ohne Limit
void thread_wake(Thread* t, int result);                // Waiting -> Ready
void reschedule();                                      // lässt einen bereiten Thread höherer Priorität laufen
void thread_yield();                                    // gibt an den Scheduler ab; der Thread bleibt bereit
int  thread_terminate(int id, bool delete_thread);      // beendet einen anderen Thread
void intr_queue_call(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2);   // Handler wie einen Interrupt ausführen
void wait_vblank();
uint32_t vblank_count();
int  intr_suspend();                                    // liefert den alten Zustand
void intr_resume(int old_state);
void request_exit(int code);                            // sceKernelExitGame, QUIT
void scheduler_run();                                   // läuft, bis kein Thread mehr lebt oder request_exit

// window.cpp (--window): Fenster, zeigt den Framebuffer
void window_init();
extern uint64_t g_perf_ge_us;                          // Zeit im GE-Renderer (Statistik)
uint32_t ctrl_buttons();
uint32_t pad_sdl_buttons();                             // alle Controller über SDL3 (pad_sdl.cpp, optional)
uint32_t pad_ps_buttons();                              // DualShock 4 / DualSense (pad_ps.cpp); Rückgabe: PSP-Tasten, Bits wie sceCtrl
void window_vblank(const uint8_t* mem);
void input_log_sound(int slot, int peak);   // aus audio_submit: Ton nach einem Tastendruck (Treffer-Ton)
void display_set_framebuf(uint32_t addr, uint32_t stride, uint32_t format);
void overlay_toggle();                 // F3: Leistungsanzeige an/aus (overlay.cpp)
void overlay_save();                   // F4: letzte 15 s der Leistungsanzeige als Text speichern
void overlay_frame();                  // neues Bild des Spiels (für FPS)
void overlay_latency(double ms);       // gemessene Latenz Taste->Bild
void name_thread(const wchar_t* name);  // Name des aktuellen Threads (Overlay-Gruppierung, Debugger)
int  exit_code();
void dump_threads();                                    // Zustand aller PSP-Threads ins Log (Kategorie THREAD)
int  new_uid();                                         // Kernel-Objekt-ID (Callbacks, Speicherblöcke, ...)
// Sub-Interrupt-Handler (InterruptManager). Bisher wird nur VBlank (Interrupt 30) ausgelöst.
int  subintr_register(int intr, int sub, uint32_t handler, uint32_t arg);
int  subintr_enable(int intr, int sub, bool enable);
int  subintr_release(int intr, int sub);
// printf-Formatierung mit Argumenten aus den Registern des Spiels (a0..a3, t0..t3); first_arg = Index des ersten Arguments
std::string guest_format(const uint8_t* mem, uint32_t fmt_addr, const PspContext* ctx, int first_arg);

// ---------------------------------------------------------------- ISO (iso.cpp)
struct IsoEntry { uint32_t lba = 0, size = 0; bool is_dir = false, found = false; };
class Iso {
public:
    bool open(const std::string& path, std::string* error);
    IsoEntry find(const std::string& path) const;       // z.B. "PSP_GAME/SYSDIR/EBOOT.BIN", ohne Laufwerk
    bool read(const IsoEntry& e, uint64_t offset, void* dst, uint32_t size) const;
    bool read_sectors(uint32_t lba, uint32_t count, void* dst) const;
    bool is_open() const { return file_ != nullptr; }
    ~Iso();
private:
    struct Node { std::string path; IsoEntry e; };
    void walk(const std::string& prefix, uint32_t lba, uint32_t size, int depth);
    void* file_ = nullptr;
    std::vector<Node> nodes_;
};
extern Iso g_iso;

// ---------------------------------------------------------------- Lader und Start (loader.cpp, run.cpp)
std::string sha1_hex(const uint8_t* data, size_t size);
void load_game();                                       // EBOOT in den Speicher laden, prüfen, Hauptthread anlegen
int run(int argc, char** argv);                         // main() der Spiel-.exe

} // namespace rt
