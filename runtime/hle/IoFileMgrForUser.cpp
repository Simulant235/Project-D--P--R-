// Datei-I/O (T24). Geräte:
//   disc0:/...     ISO, nur lesen; auch disc0:/sce_lbn0x<lba>_size0x<bytes>
//   umd0:, umd1:   ganze Disc als Blockgerät (Sektoren)
//   ms0:, fatms0:  Memory Stick = Ordner auf dem PC (--save-dir, sonst "memstick" neben der exe)
// Async-Aufrufe laufen sofort, Ergebnis kommt mit WaitAsync/PollAsync. Fehlercodes nach PPSSPP (Core/HLE/sceIo.cpp).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

#include "../src/rt.h"

using namespace rt;

namespace {

enum : uint32_t {
    ERR_NOENT = 0x80010002u, ERR_BADF = 0x80010009u, ERR_INVAL = 0x80010016u, ERR_MFILE = 0x80010018u,
    ERR_NOASYNC = 0x8002032Au, ERR_NODEV = 0x80020321u, ERR_ROFS = 0x8001001Eu
};
enum { O_RDONLY = 1, O_WRONLY = 2, O_APPEND = 0x100, O_CREAT = 0x200, O_TRUNC = 0x400 };

struct File {
    enum Kind { ISO, UMD_RAW, HOST } kind = ISO;
    IsoEntry entry;          // ISO
    FILE* host = nullptr;    // HOST
    uint64_t pos = 0, size = 0;
    std::string path;
    bool has_async = false;
    int64_t async_result = 0;
};

std::map<int, File> g_files;
std::string g_cwd = "disc0:/";
const uint32_t SECTOR = 2048;

std::string guest_str(const uint8_t* mem, uint32_t addr) {
    return addr && mem_valid(addr, 1) ? std::string((const char*)mem + addr, strnlen((const char*)mem + addr, 511)) : "";
}

// "gerät:/pfad" aus Spielpfad, relativ -> aktuelles Verzeichnis
void split(const std::string& in, std::string& dev, std::string& path) {
    std::string full = in.find(':') != std::string::npos ? in : g_cwd + (g_cwd.back() == '/' ? "" : "/") + in;
    const size_t colon = full.find(':');
    dev = full.substr(0, colon);
    for (char& c : dev)
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    path = full.substr(colon + 1);
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
}

std::filesystem::path host_path(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path root = g_opt.save_dir.empty() ? fs::path("memstick") : fs::path(g_opt.save_dir);
    fs::path p = (root / fs::path(path)).lexically_normal();
    // nicht aus dem Memory-Stick-Ordner raus
    const std::string r = root.lexically_normal().string(), s = p.string();
    if (s.compare(0, r.size(), r) != 0) return root / "_ungueltig";
    return p;
}

int new_fd() {
    for (int fd = 3; fd < 64; fd++)
        if (!g_files.count(fd)) return fd;
    return -1;
}

uint32_t do_open(const std::string& name, uint32_t flags) {
    std::string dev, path;
    split(name, dev, path);
    const int fd = new_fd();
    if (fd < 0) return ERR_MFILE;
    File f;
    f.path = name;
    if (dev == "disc0" || dev == "isofs0") {
        if (flags & O_WRONLY) return ERR_ROFS;
        unsigned lba = 0, size = 0;
        if (sscanf(path.c_str(), "sce_lbn0x%x_size0x%x", &lba, &size) == 2 || sscanf(path.c_str(), "sce_lbn%x_size%x", &lba, &size) == 2) {
            f.entry.lba = lba;
            f.entry.size = size;
            f.entry.found = true;
        } else {
            f.entry = g_iso.find(path);
            if (!f.entry.found || f.entry.is_dir) {
                psp_log(PSP_LOG_WARN, "IO", "open '%s': not found", name.c_str());
                return ERR_NOENT;
            }
        }
        f.kind = File::ISO;
        f.size = f.entry.size;
    } else if (dev == "umd0" || dev == "umd1" || dev == "umd") {
        f.kind = File::UMD_RAW;
        f.size = 0xFFFFFFFFu;
    } else if (dev == "ms0" || dev == "fatms0") {
        const std::filesystem::path hp = host_path(path);
        const char* mode = "rb";
        if (flags & O_WRONLY) {
            std::error_code ec;
            std::filesystem::create_directories(hp.parent_path(), ec);
            const bool exists = std::filesystem::exists(hp, ec);
            if (!exists && !(flags & O_CREAT)) return ERR_NOENT;
            mode = (flags & O_TRUNC) || !exists ? ((flags & O_RDONLY) ? "w+b" : "wb") : "r+b";
        }
        f.host = fopen(hp.string().c_str(), mode);
        if (!f.host) {
            psp_log(PSP_LOG_INFO, "IO", "open '%s': not found", name.c_str());
            return ERR_NOENT;
        }
        _fseeki64(f.host, 0, SEEK_END);
        f.size = (uint64_t)_ftelli64(f.host);
        _fseeki64(f.host, 0, SEEK_SET);
        if (flags & O_APPEND) f.pos = f.size;
        f.kind = File::HOST;
    } else {
        psp_log(PSP_LOG_WARN, "IO", "open '%s': unknown device", name.c_str());
        return ERR_NODEV;
    }
    psp_log(PSP_LOG_DEBUG, "IO", "open '%s' fd=%d size=0x%llX", name.c_str(), fd, (unsigned long long)f.size);
    g_files[fd] = f;
    return (uint32_t)fd;
}

int64_t do_read(File& f, uint8_t* mem, uint32_t buf, uint32_t size) {
    psp_log(PSP_LOG_VERBOSE, "IO", "read %s pos=0x%llX size=0x%X", f.path.c_str(), (unsigned long long)f.pos, size);
    if (!size) return 0;
    if (f.kind == File::UMD_RAW) {   // size in Sektoren
        if (!mem_valid(buf, size * SECTOR)) return (int32_t)ERR_INVAL;
        if (!g_iso.read_sectors((uint32_t)f.pos, size, mem + buf)) return (int32_t)ERR_INVAL;
        f.pos += size;
        return size;
    }
    if (f.pos >= f.size) return 0;
    const uint32_t n = (uint32_t)std::min<uint64_t>(size, f.size - f.pos);
    if (!mem_valid(buf, n)) return (int32_t)ERR_INVAL;
    if (f.kind == File::ISO) {
        if (!g_iso.read(f.entry, f.pos, mem + buf, n)) return (int32_t)ERR_INVAL;
    } else {
        _fseeki64(f.host, (long long)f.pos, SEEK_SET);
        if (fread(mem + buf, 1, n, f.host) != n) return (int32_t)ERR_INVAL;
    }
    f.pos += n;
    return n;
}

int64_t do_seek(File& f, int64_t offset, uint32_t whence) {
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f.pos : (int64_t)f.size;
    if (whence > 2) return (int32_t)ERR_INVAL;
    int64_t p = base + offset;
    if (p < 0) return (int32_t)ERR_INVAL;
    f.pos = (uint64_t)p;
    return p;
}

File* file(uint32_t fd) {
    auto it = g_files.find((int)fd);
    return it == g_files.end() ? nullptr : &it->second;
}

void write_async_result(uint8_t* mem, uint32_t ptr, int64_t v) {
    if (ptr && mem_valid(ptr, 8)) {
        psp_w32(mem, ptr, (uint32_t)v);
        psp_w32(mem, ptr + 4, (uint32_t)((uint64_t)v >> 32));
    }
}

} // namespace

extern "C" {

// SceUID sceIoOpen(const char* path, int flags, SceMode mode)
PSP_HLE(IoFileMgrForUser, 0x109F50BC) { HLE_RET(do_open(guest_str(mem, HLE_ARG(0)), HLE_ARG(1))); }

// int sceIoClose(SceUID fd)
PSP_HLE(IoFileMgrForUser, 0x810C4BC3) {
    (void)mem;
    File* f = file(HLE_ARG(0));
    if (!f) { HLE_RET(ERR_BADF); return; }
    if (f->host) fclose(f->host);
    g_files.erase((int)HLE_ARG(0));
    HLE_RET(0);
}

// int sceIoRead(SceUID fd, void* buf, u32 size)
PSP_HLE(IoFileMgrForUser, 0x6A638D83) {
    File* f = file(HLE_ARG(0));
    if (!f) { HLE_RET(ERR_BADF); return; }
    HLE_RET((uint32_t)do_read(*f, mem, HLE_ARG(1), HLE_ARG(2)));
}

// int sceIoWrite(SceUID fd, const void* buf, u32 size)   (fd 1/2 = Textausgabe ins Log)
PSP_HLE(IoFileMgrForUser, 0x42EC03AC) {
    const uint32_t fd = HLE_ARG(0), buf = HLE_ARG(1), size = HLE_ARG(2);
    if (!mem_valid(buf, size)) { HLE_RET(ERR_INVAL); return; }
    if (fd == 1 || fd == 2) {
        std::string text((const char*)mem + buf, size);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == 0)) text.pop_back();
        if (!text.empty()) psp_log(PSP_LOG_INFO, "GAME", "%s", text.c_str());
        HLE_RET(size);
        return;
    }
    File* f = file(fd);
    if (!f || f->kind != File::HOST) { HLE_RET(ERR_BADF); return; }
    _fseeki64(f->host, (long long)f->pos, SEEK_SET);
    const size_t n = fwrite(mem + buf, 1, size, f->host);
    fflush(f->host);
    f->pos += n;
    if (f->pos > f->size) f->size = f->pos;
    HLE_RET((uint32_t)n);
}

// SceOff sceIoLseek(SceUID fd, SceOff offset, int whence)   (offset in a2:a3, whence in t0)
PSP_HLE(IoFileMgrForUser, 0x27EB27B8) {
    (void)mem;
    File* f = file(HLE_ARG(0));
    if (!f) { HLE_RET64((int64_t)(int32_t)ERR_BADF); return; }
    HLE_RET64(do_seek(*f, (int64_t)HLE_ARG64(2), HLE_ARG(4)));
}

// int sceIoLseekAsync(SceUID fd, SceOff offset, int whence)
PSP_HLE(IoFileMgrForUser, 0x71B19E77) {
    (void)mem;
    File* f = file(HLE_ARG(0));
    if (!f) { HLE_RET(ERR_BADF); return; }
    f->async_result = do_seek(*f, (int64_t)HLE_ARG64(2), HLE_ARG(4));
    f->has_async = true;
    HLE_RET(0);
}

// int sceIoReadAsync(SceUID fd, void* buf, u32 size)
PSP_HLE(IoFileMgrForUser, 0xA0B5A7C2) {
    File* f = file(HLE_ARG(0));
    if (!f) { HLE_RET(ERR_BADF); return; }
    f->async_result = do_read(*f, mem, HLE_ARG(1), HLE_ARG(2));
    f->has_async = true;
    HLE_RET(0);
}

// int sceIoWaitAsync(SceUID fd, SceInt64* result) / sceIoWaitAsyncCB / sceIoPollAsync
static void io_async_result(PspContext* ctx, uint8_t* mem) {
    File* f = file(HLE_ARG(0));
    if (!f) { HLE_RET(ERR_BADF); return; }
    if (!f->has_async) { HLE_RET(ERR_NOASYNC); return; }
    write_async_result(mem, HLE_ARG(1), f->async_result);
    f->has_async = false;
    HLE_RET(0);
}
PSP_HLE(IoFileMgrForUser, 0xE23EEC33) { io_async_result(ctx, mem); }
PSP_HLE(IoFileMgrForUser, 0x35DBD746) { io_async_result(ctx, mem); }
PSP_HLE(IoFileMgrForUser, 0x3251EA56) { io_async_result(ctx, mem); }

PSP_HLE(IoFileMgrForUser, 0xB293727F) { (void)mem; HLE_RET(0); }   // sceIoChangeAsyncPriority

// int sceIoChdir(const char* path)
PSP_HLE(IoFileMgrForUser, 0x55F4717D) {
    const std::string p = guest_str(mem, HLE_ARG(0));
    if (p.find(':') != std::string::npos) g_cwd = p;
    else g_cwd += (g_cwd.back() == '/' ? "" : "/") + p;
    psp_log(PSP_LOG_INFO, "IO", "chdir '%s'", g_cwd.c_str());
    HLE_RET(0);
}

// int sceIoGetstat(const char* path, SceIoStat* stat)
PSP_HLE(IoFileMgrForUser, 0xACE946E8) {
    const std::string name = guest_str(mem, HLE_ARG(0));
    const uint32_t st = HLE_ARG(1);
    std::string dev, path;
    split(name, dev, path);
    uint64_t size = 0;
    bool dir = false, found = false;
    uint32_t lba = 0;
    if (dev == "disc0") {
        const IsoEntry e = g_iso.find(path);
        found = e.found;
        dir = e.is_dir;
        size = e.size;
        lba = e.lba;
    } else if (dev == "ms0" || dev == "fatms0") {
        std::error_code ec;
        const auto hp = host_path(path);
        found = std::filesystem::exists(hp, ec);
        dir = found && std::filesystem::is_directory(hp, ec);
        if (found && !dir) size = std::filesystem::file_size(hp, ec);
    }
    if (!found) { HLE_RET(ERR_NOENT); return; }
    if (mem_valid(st, 88)) {
        memset(mem + st, 0, 88);
        psp_w32(mem, st, dir ? 0x11FFu : 0x21FFu);         // st_mode: Dir bzw. Datei, alle Rechte
        psp_w32(mem, st + 4, dir ? 0x10u : 0x20u);         // st_attr
        psp_w32(mem, st + 8, (uint32_t)size);
        psp_w32(mem, st + 12, (uint32_t)(size >> 32));
        psp_w32(mem, st + 64, lba);                        // st_private[0]: erster Sektor
    }
    HLE_RET(0);
}

// int sceIoDevctl(const char* dev, u32 cmd, void* in, int inLen, void* out, int outLen)
PSP_HLE(IoFileMgrForUser, 0x54F5FB11) {
    const std::string dev = guest_str(mem, HLE_ARG(0));
    const uint32_t cmd = HLE_ARG(1), out = HLE_ARG(4), out_len = HLE_ARG(5);
    psp_log(PSP_LOG_DEBUG, "IO", "devctl '%s' cmd=0x%08X in=0x%08X/%u out=0x%08X/%u ra=0x%08X", dev.c_str(), cmd, HLE_ARG(2), HLE_ARG(3), out, out_len,
            ctx->r[PSP_RA]);
    switch (cmd) {
    case 0x02025806:   // Memory Stick eingelegt?
    case 0x02425823:   // FAT-Treiber: Memory Stick bereit?
        if (out && out_len >= 4 && mem_valid(out, 4)) psp_w32(mem, out, 1);
        break;
    case 0x01F20001:   // UMD: Disc-Typ {?, Typ}
        if (out && out_len >= 8 && mem_valid(out, 8)) {
            psp_w32(mem, out, 0xFFFFFFFFu);
            psp_w32(mem, out + 4, 0x10);   // Spiel-Disc
        }
        break;
    default: break;   // Rest (Callbacks an/abmelden o.ä.): Erfolg, sonst nichts
    }
    HLE_RET(0);
}

} // extern "C"
