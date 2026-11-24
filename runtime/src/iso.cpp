// ISO 9660, nur lesen. Basis für Lader und später IoFileMgr
#include <cstdio>
#include <cstring>

#include "rt.h"

namespace rt {

Iso g_iso;

namespace {
const uint32_t SECTOR = 2048;
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return s;
}
} // namespace

Iso::~Iso() {
    if (file_) fclose((FILE*)file_);
}

bool Iso::read_sectors(uint32_t lba, uint32_t count, void* dst) const {
    if (!file_) return false;
    if (_fseeki64((FILE*)file_, (long long)lba * SECTOR, SEEK_SET) != 0) return false;
    return fread(dst, SECTOR, count, (FILE*)file_) == count;
}

bool Iso::read(const IsoEntry& e, uint64_t offset, void* dst, uint32_t size) const {
    if (!file_ || !e.found || offset + size > e.size) return false;
    if (_fseeki64((FILE*)file_, (long long)e.lba * SECTOR + (long long)offset, SEEK_SET) != 0) return false;
    return fread(dst, 1, size, (FILE*)file_) == size;
}

void Iso::walk(const std::string& prefix, uint32_t lba, uint32_t size, int depth) {
    if (depth > 8 || size > 0x400000) return;
    std::vector<uint8_t> data(size);
    if (_fseeki64((FILE*)file_, (long long)lba * SECTOR, SEEK_SET) != 0) return;
    if (fread(data.data(), 1, size, (FILE*)file_) != size) return;
    for (size_t pos = 0; pos < data.size();) {
        const uint8_t len = data[pos];
        if (len == 0) {   // Rest des Sektors ist Auffüllung
            pos = (pos / SECTOR + 1) * SECTOR;
            continue;
        }
        if (pos + len > data.size() || len < 34) break;
        const uint8_t* rec = &data[pos];
        pos += len;
        const uint8_t name_len = rec[32];
        if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) continue;   // "." und ".."
        std::string name((const char*)rec + 33, name_len);
        const size_t semi = name.find(';');
        if (semi != std::string::npos) name.resize(semi);
        if (!name.empty() && name.back() == '.') name.pop_back();
        Node n;
        n.path = upper(prefix + name);
        n.e.lba = le32(rec + 2);
        n.e.size = le32(rec + 10);
        n.e.is_dir = (rec[25] & 2) != 0;
        n.e.found = true;
        nodes_.push_back(n);
        if (n.e.is_dir) walk(prefix + name + "/", n.e.lba, n.e.size, depth + 1);
    }
}

bool Iso::open(const std::string& path, std::string* error) {
    file_ = fopen(path.c_str(), "rb");
    if (!file_) {
        if (error) *error = "ISO nicht gefunden: " + path;
        return false;
    }
    uint8_t pvd[SECTOR];
    if (!read_sectors(16, 1, pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        if (error) *error = "keine ISO-9660-Datei (komprimierte CSO-Dateien zuerst umwandeln): " + path;
        fclose((FILE*)file_);
        file_ = nullptr;
        return false;
    }
    walk("", le32(pvd + 156 + 2), le32(pvd + 156 + 10), 0);
    return true;
}

IsoEntry Iso::find(const std::string& path) const {
    std::string p = path;
    for (char& c : p)
        if (c == '\\') c = '/';
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (!p.empty() && p.back() == '/') p.pop_back();
    p = upper(p);
    for (const Node& n : nodes_)
        if (n.path == p) return n.e;
    return IsoEntry{};
}

} // namespace rt
