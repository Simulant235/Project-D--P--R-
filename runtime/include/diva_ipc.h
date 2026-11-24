/*
 * IPC zwischen MM+-Mod (DLL) und PSP-Prozess (.exe). Von mod/ und runtime/ eingebunden.
 * Reines C, feste Größen, keine Zeiger.
 */
#ifndef DIVA_IPC_H
#define DIVA_IPC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIVA_IPC_MAGIC    0x41564944u   /* "DIVA" */
#define DIVA_IPC_VERSION  1u

/* Namen der Kernel-Objekte; <id> ist die Zeichenkette aus --ipc (von der Mod frei gewählt, z.B. "<pid>-<zufall>") */
#define DIVA_IPC_NAME_SHM      "Local\\DivaPspRecomp-%s-shm"      /* Shared Memory mit DivaIpcShared */
#define DIVA_IPC_NAME_TEXTURE  "Local\\DivaPspRecomp-%s-tex-%u"   /* Shared-Texture-Handle, %u = texture_generation */
#define DIVA_IPC_NAME_FRAME    "Local\\DivaPspRecomp-%s-frame"    /* Auto-Reset-Event: neues Bild liegt vor */
#define DIVA_IPC_NAME_WAKE     "Local\\DivaPspRecomp-%s-wake"     /* Auto-Reset-Event: Mod hat command geändert */

typedef enum DivaIpcState {
    DIVA_STATE_NONE = 0,      /* Prozess noch nicht verbunden */
    DIVA_STATE_STARTING,      /* verbunden, lädt */
    DIVA_STATE_RUNNING,
    DIVA_STATE_PAUSED,
    DIVA_STATE_EXITING,       /* beendet sich geordnet */
    DIVA_STATE_FAILED         /* Fehler; error_code und error_text sind gesetzt */
} DivaIpcState;

typedef enum DivaIpcCommand {
    DIVA_CMD_NONE = 0,
    DIVA_CMD_PAUSE,
    DIVA_CMD_RESUME,
    DIVA_CMD_QUIT             /* geordnet beenden (Spielstand ist Sache des Spiels) */
} DivaIpcCommand;

/* Tasten wie PSP_CTRL_* der PSP-Firmware */
enum {
    DIVA_BTN_SELECT = 0x0001, DIVA_BTN_START = 0x0008,
    DIVA_BTN_UP = 0x0010, DIVA_BTN_RIGHT = 0x0020, DIVA_BTN_DOWN = 0x0040, DIVA_BTN_LEFT = 0x0080,
    DIVA_BTN_LTRIGGER = 0x0100, DIVA_BTN_RTRIGGER = 0x0200,
    DIVA_BTN_TRIANGLE = 0x1000, DIVA_BTN_CIRCLE = 0x2000, DIVA_BTN_CROSS = 0x4000, DIVA_BTN_SQUARE = 0x8000
};

#pragma pack(push, 4)

/* Mod -> PSP-Prozess. Die Mod schreibt, der Prozess liest. */
typedef struct DivaIpcInput {
    volatile uint32_t seq;          /* Seqlock: ungerade = wird gerade geschrieben; Leser wiederholt bei Änderung */
    uint32_t buttons;               /* DIVA_BTN_* */
    uint8_t  analog_x, analog_y;    /* 0..255, Mitte 128 */
    uint8_t  reserved[2];
} DivaIpcInput;

typedef struct DivaIpcShared {
    /* Kopf: von der Mod vor dem Prozessstart gesetzt, danach unverändert */
    uint32_t magic;                 /* DIVA_IPC_MAGIC */
    uint32_t version;               /* DIVA_IPC_VERSION */
    uint32_t struct_size;           /* sizeof(DivaIpcShared) */
    uint32_t parent_pid;
    int64_t  adapter_luid;          /* DXGI-Adapter von MM+; der Prozess muss sein Device auf demselben Adapter anlegen */
    uint32_t max_width, max_height; /* Größe des MM+-Backbuffers als Obergrenze für die Zieltextur */
    uint32_t resolution_scale;      /* Auflösungsmultiplikator 1..6 (480x272 mal n) */
    uint32_t reserved_head[7];

    /* Mod -> Prozess */
    volatile uint32_t command;      /* DivaIpcCommand */
    volatile uint32_t command_seq;  /* von der Mod bei jedem neuen Kommando erhöht */
    volatile uint32_t mod_heartbeat;/* von der Mod in jedem MM+-Frame erhöht */
    DivaIpcInput      input;
    uint32_t reserved_in[8];

    /* Prozess -> Mod */
    volatile uint32_t state;        /* DivaIpcState */
    volatile uint32_t command_ack;  /* zuletzt verarbeitete command_seq */
    volatile uint32_t heartbeat;    /* vom Prozess in jedem VBlank erhöht */
    volatile uint32_t frame_counter;/* nach jedem fertig kopierten Bild erhöht */
    volatile uint32_t texture_generation; /* 0 = noch keine Textur; erhöht, wenn die Textur neu angelegt wurde */
    uint32_t texture_width, texture_height;
    uint32_t texture_format;        /* DXGI_FORMAT, fest DXGI_FORMAT_R8G8B8A8_UNORM (28) */
    uint32_t error_code;            /* Exit-Code */
    char     error_text[256];       /* UTF-8, nullterminiert */
    char     runtime_version[32];   /* Inhalt von VERSION */
    uint32_t reserved_out[8];
} DivaIpcShared;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* DIVA_IPC_H */
