// Interrupts sperren/freigeben (T20). Dazwischen kein Thread-Wechsel, nichts wird zugestellt.
#include "../src/rt.h"

extern "C" {

// int sceKernelCpuSuspendIntr(void): liefert den alten Zustand (1 = Interrupts waren an)
PSP_HLE(Kernel_Library, 0x092968F4) {
    (void)mem;
    HLE_RET(rt::intr_suspend() ? 0 : 1);
}

// void sceKernelCpuResumeIntr(int flags)
PSP_HLE(Kernel_Library, 0x5F10D406) {
    (void)mem;
    rt::intr_resume(HLE_ARG(0) ? 0 : 1);
    HLE_RET(0);
    rt::reschedule();
}

} // extern "C"
