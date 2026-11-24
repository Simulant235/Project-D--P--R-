// Alle Controller über SDL3 (PS, Xbox, Switch, No-Name). Nur gebaut mit third_party/SDL3 (py tools/fetch_sdl3.py).
// Belegung nach Position: unten = Kreuz, rechts = Kreis, links = Quadrat, oben = Dreieck,
// Start = START, Back/Share/Minus = SELECT, Schultern = L/R, Steuerkreuz + linker Stick.
#include <SDL3/SDL.h>

#include "rt.h"

namespace rt {

namespace {
bool g_init = false, g_failed = false;
SDL_Gamepad* g_pad = nullptr;
uint64_t g_next_scan = 0;
} // namespace

uint32_t pad_sdl_buttons() {
    if (g_failed) return 0;
    if (!g_init) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   // Fenster gehört nicht SDL
        if (!SDL_Init(SDL_INIT_GAMEPAD)) {
            psp_log(PSP_LOG_WARN, "PAD", "SDL_Init fehlgeschlagen: %s", SDL_GetError());
            g_failed = true;
            return 0;
        }
        g_init = true;
    }
    const uint64_t now = time_us();
    if (now >= g_next_scan) {   // An- und Abstecken erkennen (10x pro Sekunde)
        g_next_scan = now + 100000;
        SDL_PumpEvents();
        if (g_pad && !SDL_GamepadConnected(g_pad)) {
            psp_log(PSP_LOG_INFO, "PAD", "Controller getrennt");
            SDL_CloseGamepad(g_pad);
            g_pad = nullptr;
        }
        if (!g_pad) {
            int n = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&n);
            if (ids && n > 0) {
                g_pad = SDL_OpenGamepad(ids[0]);
                if (g_pad) psp_log(PSP_LOG_INFO, "PAD", "Controller: %s", SDL_GetGamepadName(g_pad));
            }
            SDL_free(ids);
        }
    }
    if (!g_pad) return 0;
    SDL_UpdateGamepads();
    static const struct { SDL_GamepadButton b; uint32_t bit; } map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x4000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000}, {SDL_GAMEPAD_BUTTON_WEST, 0x8000},
        {SDL_GAMEPAD_BUTTON_NORTH, 0x1000}, {SDL_GAMEPAD_BUTTON_START, 0x8}, {SDL_GAMEPAD_BUTTON_BACK, 0x1},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x10}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x20}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x40},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x80}};
    uint32_t b = 0;
    for (const auto& m : map)
        if (SDL_GetGamepadButton(g_pad, m.b)) b |= m.bit;
    const int lx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (ly < -16000) b |= 0x10;
    if (ly > 16000) b |= 0x40;
    if (lx > 16000) b |= 0x20;
    if (lx < -16000) b |= 0x80;
    return b;
}

} // namespace rt
