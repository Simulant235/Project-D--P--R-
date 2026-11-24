// DualShock 4 / DualSense (Edge) per USB oder Bluetooth direkt als HID gelesen (T32).
// Eigener Thread nimmt den ersten Sony-Controller, Tasten landen als PSP-Bits.
// L1/R1 = L/R, OPTIONS = START, SHARE/CREATE = SELECT, Steuerkreuz + linker Stick = Steuerkreuz.
// Berichtsformate nach hid-sony.c / hid-playstation.c (Linux).
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <atomic>
#include <thread>

#include "rt.h"

namespace rt {

namespace {

std::atomic<uint32_t> g_pad{0};
std::atomic<bool> g_started{false};

enum Kind { NONE, DS4, DUALSENSE };

Kind kind_of(USHORT vid, USHORT pid) {
    if (vid != 0x054C) return NONE;
    if (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0) return DS4;   // DualShock 4 (v1, v2, USB-Adapter)
    if (pid == 0x0CE6 || pid == 0x0DF2) return DUALSENSE;              // DualSense, DualSense Edge
    return NONE;
}

HANDLE open_controller(Kind* kind) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof ifd};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)malloc(need);
        if (!det) continue;
        det->cbSize = sizeof *det;
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) {
            HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES a = {sizeof a};
                const Kind k = HidD_GetAttributes(h, &a) ? kind_of(a.VendorID, a.ProductID) : NONE;
                if (k != NONE) { found = h; *kind = k; }
                else CloseHandle(h);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

uint32_t dpad(int d) {   // 0 = oben, im Uhrzeigersinn, 8 = nichts
    static const uint32_t m[8] = {0x10, 0x10 | 0x20, 0x20, 0x20 | 0x40, 0x40, 0x40 | 0x80, 0x80, 0x80 | 0x10};
    return d < 8 ? m[d] : 0;
}

// sticks: lx, ly; face: Byte mit Steuerkreuz (Bits 0-3) und Symboltasten (4-7); sh: L1/R1/.../SHARE/OPTIONS
uint32_t decode(uint8_t lx, uint8_t ly, uint8_t face, uint8_t sh) {
    uint32_t b = dpad(face & 15);
    if (face & 0x10) b |= 0x8000;   // Quadrat
    if (face & 0x20) b |= 0x4000;   // Kreuz
    if (face & 0x40) b |= 0x2000;   // Kreis
    if (face & 0x80) b |= 0x1000;   // Dreieck
    if (sh & 0x01) b |= 0x100;      // L1
    if (sh & 0x02) b |= 0x200;      // R1
    if (sh & 0x10) b |= 0x1;        // SHARE / CREATE = SELECT
    if (sh & 0x20) b |= 0x8;        // OPTIONS = START
    if (ly < 64) b |= 0x10;
    if (ly > 192) b |= 0x40;
    if (lx < 64) b |= 0x80;
    if (lx > 192) b |= 0x20;
    return b;
}

void reader() {
    for (;;) {
        Kind kind = NONE;
        HANDLE h = open_controller(&kind);
        if (h == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
        psp_log(PSP_LOG_INFO, "PAD", "PlayStation-Controller gefunden (%s)", kind == DS4 ? "DualShock 4" : "DualSense");
        uint8_t r[1024];
        DWORD n = 0;
        while (ReadFile(h, r, sizeof r, &n, nullptr) && n > 0) {
            if (kind == DS4) {
                if (r[0] == 0x01 && n >= 8) g_pad = decode(r[1], r[2], r[5], r[6]);                   // USB / BT einfach
                else if (r[0] == 0x11 && n >= 10) g_pad = decode(r[3], r[4], r[7], r[8]);             // Bluetooth voll
            } else {
                if (r[0] == 0x01 && n >= 64) g_pad = decode(r[1], r[2], r[8], r[9]);                  // USB
                else if (r[0] == 0x31 && n >= 11) g_pad = decode(r[2], r[3], r[9], r[10]);            // Bluetooth voll
                else if (r[0] == 0x01 && n >= 8) g_pad = decode(r[1], r[2], r[5], r[6]);              // Bluetooth einfach
            }
        }
        g_pad = 0;
        CloseHandle(h);
        psp_log(PSP_LOG_INFO, "PAD", "Controller getrennt");
    }
}

} // namespace

uint32_t pad_ps_buttons() {
    if (!g_started.exchange(true)) std::thread(reader).detach();
    return g_pad;
}

} // namespace rt
