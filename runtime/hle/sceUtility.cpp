// sceUtility (T24): Module, Systemparameter, Dialoge. Dialoge noch ohne Oberfläche, melden sofort "fertig".
// Savedata als Datei (savedata()), PARAM.SFO/Icons/Größenabfragen fehlen.
// Dialogstatus wie PSP: 0 NONE, 1 INITIALIZE, 2 RUNNING, 3 FINISHED, 4 SHUTDOWN
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <string>
#include <vector>

#include "../src/rt.h"

using namespace rt;

namespace {

enum Dialog { MSG, OSK, NETCONF, SCREENSHOT, SAVEDATA, DIALOG_COUNT };
int g_status[DIALOG_COUNT] = {};
const uint32_t RESULT_OFFSET = 0x1C;                    // SceUtilityDialogCommon::result
const uint32_t ERROR_LOAD_NO_DATA = 0x80110306u;
const uint32_t ERROR_INVALID_STATUS = 0x80110001u;

// ---- Savedata (SceUtilitySavedataParam, Offsets nach PPSSPP Core/Dialog/SavedataParam.h)
// Pfad: <save-dir>/<gameName><saveName>/<fileName>, Default "saves"
enum : uint32_t { SD_MODE = 0x30, SD_GAME = 0x3C, SD_SAVE = 0x4C, SD_LIST = 0x60, SD_FILE = 0x64, SD_BUF = 0x74,
                  SD_BUFSIZE = 0x78, SD_SIZE = 0x7C };

std::string cstr(const uint8_t* mem, uint32_t a, uint32_t max) {
    std::string r;
    for (uint32_t i = 0; i < max && mem_valid(a + i, 1) && mem[a + i]; i++) r += (char)mem[a + i];
    return r;
}

std::string save_path(const std::string& game, const std::string& save, const std::string& file) {
    const std::string dir = (g_opt.save_dir.empty() ? std::string("saves") : g_opt.save_dir) + "/" + game + save;
    return file.empty() ? dir : dir + "/" + file;
}

uint32_t savedata(uint8_t* mem, uint32_t p) {
    const uint32_t mode = psp_r32(mem, p + SD_MODE);
    const std::string game = cstr(mem, p + SD_GAME, 13), file = cstr(mem, p + SD_FILE, 13);
    std::string save = cstr(mem, p + SD_SAVE, 20);
    const bool list = mode == 4 || mode == 5;
    std::vector<std::string> names;
    if (list && mem_valid(psp_r32(mem, p + SD_LIST), 20))
        for (uint32_t a = psp_r32(mem, p + SD_LIST); names.size() < 100; a += 20) {
            const std::string n = cstr(mem, a, 20);
            if (n.empty()) break;
            names.push_back(n);
        }
    const bool load = mode == 0 || mode == 2 || mode == 4, store = mode == 1 || mode == 3 || mode == 5;
    if (list && !names.empty()) {
        save = names[0];
        if (load)
            for (const auto& n : names) {
                FILE* t = fopen(save_path(game, n, file).c_str(), "rb");
                if (t) { fclose(t); save = n; break; }
            }
        for (uint32_t i = 0; i < 20; i++) psp_w8(mem, p + SD_SAVE + i, i < save.size() ? (uint8_t)save[i] : 0);
    }
    const std::string path = save_path(game, save, file);
    const uint32_t buf = psp_r32(mem, p + SD_BUF), bufsize = psp_r32(mem, p + SD_BUFSIZE), size = psp_r32(mem, p + SD_SIZE);
    psp_log(PSP_LOG_INFO, "SAVE", "mode=%u game='%s' save='%s' file='%s' liste=%zu puffer=0x%08X/%u groesse=%u", mode, game.c_str(),
            save.c_str(), file.c_str(), names.size(), buf, bufsize, size);
    if (load) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return ERROR_LOAD_NO_DATA;
        size_t n = 0;
        if (buf && mem_valid(buf, bufsize)) n = fread(mem + buf, 1, bufsize, f);
        fclose(f);
        psp_w32(mem, p + SD_SIZE, (uint32_t)n);
        psp_log(PSP_LOG_INFO, "SAVE", "geladen: %s (%zu Byte)", path.c_str(), n);
        return 0;
    }
    if (store) {
        if (!buf || !mem_valid(buf, size)) return 0;
        std::error_code ec;
        std::filesystem::create_directories(save_path(game, save, ""), ec);
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { psp_log(PSP_LOG_WARN, "SAVE", "kann %s nicht schreiben", path.c_str()); return 0; }
        fwrite(mem + buf, 1, size, f);
        fclose(f);
        psp_log(PSP_LOG_INFO, "SAVE", "gespeichert: %s (%u Byte)", path.c_str(), size);
    }
    return 0;
}

void init_dialog(PspContext* ctx, uint8_t* mem, Dialog d, const char* name) {
    const uint32_t params = HLE_ARG(0);
    uint32_t result = 0;
    if (params && mem_valid(params, 0x80)) {
        if (d == SAVEDATA) result = savedata(mem, params);
        psp_w32(mem, params + RESULT_OFFSET, result);
    }
    psp_log(PSP_LOG_INFO, "HLE", "%s init", name);
    g_status[d] = 2;
    HLE_RET(0);
}

void update_dialog(PspContext* ctx, Dialog d) {
    if (g_status[d] == 2) g_status[d] = 3;
    HLE_RET(0);
}

void shutdown_dialog(PspContext* ctx, Dialog d) {
    if (g_status[d] != 3 && g_status[d] != 2) { HLE_RET(ERROR_INVALID_STATUS); return; }
    g_status[d] = 4;
    HLE_RET(0);
}

void status_dialog(PspContext* ctx, Dialog d) {
    HLE_RET(g_status[d]);
    if (g_status[d] == 4) g_status[d] = 0;   // SHUTDOWN nur einmal melden
}

} // namespace

extern "C" {

// ---------------------------------------------------------------- Module

PSP_HLE(sceUtility, 0x2A2B3DE0) { (void)mem; HLE_RET(0); }   // sceUtilityLoadModule, Libs sind eingebaut
PSP_HLE(sceUtility, 0xE49BFE92) { (void)mem; HLE_RET(0); }   // sceUtilityUnloadModule

// ---------------------------------------------------------------- Systemparameter

// int sceUtilityGetSystemParamInt(int id, int* value)
PSP_HLE(sceUtility, 0xA5DA2406) {
    const uint32_t id = HLE_ARG(0), out = HLE_ARG(1);
    int32_t v = 0;
    switch (id) {
    case 1: v = 0; break;    // Ad-hoc-Kanal
    case 2: v = 0; break;    // WLAN-Energiesparen
    case 3: v = 0; break;    // Datumsformat: JJJJ-MM-TT
    case 4: v = 1; break;    // Zeitformat: 24 Stunden
    case 5: v = 0; break;    // Zeitzone (Minuten)
    case 6: v = 0; break;    // Sommerzeit
    case 7: v = 0; break;    // Sprache: Japanisch
    case 8: v = 0; break;    // Bestätigungstaste: Kreis
    default: HLE_RET(0x80110103u); return;
    }
    if (out && mem_valid(out, 4)) psp_w32(mem, out, (uint32_t)v);
    HLE_RET(0);
}

// int sceUtilityGetSystemParamString(int id, char* buf, int len)
PSP_HLE(sceUtility, 0x34B78343) {
    const uint32_t id = HLE_ARG(0), buf = HLE_ARG(1), len = HLE_ARG(2);
    if (id != 1) { HLE_RET(0x80110103u); return; }
    if (buf && len && mem_valid(buf, len)) {
        memset(mem + buf, 0, len);
        memcpy(mem + buf, "DIVA", len > 4 ? 4 : len - 1);
    }
    HLE_RET(0);
}

// ---------------------------------------------------------------- Dialoge

PSP_HLE(sceUtility, 0x50C4CD57) { init_dialog(ctx, mem, SAVEDATA, "savedata"); }   // sceUtilitySavedataInitStart
PSP_HLE(sceUtility, 0xD4B95FFB) { (void)mem; update_dialog(ctx, SAVEDATA); }       // sceUtilitySavedataUpdate
PSP_HLE(sceUtility, 0x8874DBE0) { (void)mem; status_dialog(ctx, SAVEDATA); }       // sceUtilitySavedataGetStatus
PSP_HLE(sceUtility, 0x9790B33C) { (void)mem; shutdown_dialog(ctx, SAVEDATA); }     // sceUtilitySavedataShutdownStart

PSP_HLE(sceUtility, 0x2AD8E239) { init_dialog(ctx, mem, MSG, "msg dialog"); }      // sceUtilityMsgDialogInitStart
PSP_HLE(sceUtility, 0x95FC253B) { (void)mem; update_dialog(ctx, MSG); }
PSP_HLE(sceUtility, 0x9A1C91D7) { (void)mem; status_dialog(ctx, MSG); }
PSP_HLE(sceUtility, 0x67AF3428) { (void)mem; shutdown_dialog(ctx, MSG); }

PSP_HLE(sceUtility, 0xF6269B82) { init_dialog(ctx, mem, OSK, "osk"); }             // sceUtilityOskInitStart
PSP_HLE(sceUtility, 0x4B85C861) { (void)mem; update_dialog(ctx, OSK); }
PSP_HLE(sceUtility, 0xF3F76017) { (void)mem; status_dialog(ctx, OSK); }
PSP_HLE(sceUtility, 0x3DFAEBA9) { (void)mem; shutdown_dialog(ctx, OSK); }

PSP_HLE(sceUtility, 0x4DB1E739) { init_dialog(ctx, mem, NETCONF, "netconf"); }     // sceUtilityNetconfInitStart
PSP_HLE(sceUtility, 0x91E70E35) { (void)mem; update_dialog(ctx, NETCONF); }
PSP_HLE(sceUtility, 0x6332AA39) { (void)mem; status_dialog(ctx, NETCONF); }
PSP_HLE(sceUtility, 0xF88155F6) { (void)mem; shutdown_dialog(ctx, NETCONF); }

PSP_HLE(sceUtility, 0x0251B134) { init_dialog(ctx, mem, SCREENSHOT, "screenshot"); }   // sceUtilityScreenshotInitStart
PSP_HLE(sceUtility, 0xAB083EA9) { (void)mem; update_dialog(ctx, SCREENSHOT); }
PSP_HLE(sceUtility, 0xD81957B7) { (void)mem; status_dialog(ctx, SCREENSHOT); }
PSP_HLE(sceUtility, 0xF9E0008C) { (void)mem; shutdown_dialog(ctx, SCREENSHOT); }

} // extern "C"
