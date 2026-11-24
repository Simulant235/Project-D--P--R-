#!/usr/bin/env python3
"""Prüfsummen und Inhalt einer PSP-ISO (ISO 9660). Nur Stdlib, schreibt nur bei --extract
    py tools/iso_info.py game/diva1st.iso
    py tools/iso_info.py game/diva1st.iso --json > generated/analysis/iso.json
    py tools/iso_info.py game/diva1st.iso --no-hash --depth 2
    py tools/iso_info.py game/diva1st.iso --extract PSP_GAME/PARAM.SFO game/extracted/PARAM.SFO
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

SECTOR = 2048
CHUNK = 4 * 1024 * 1024


# ---------------------------------------------------------------- ISO 9660

class IsoFile:
    __slots__ = ("path", "lba", "size", "is_dir")

    def __init__(self, path: str, lba: int, size: int, is_dir: bool):
        self.path, self.lba, self.size, self.is_dir = path, lba, size, is_dir


class Iso:
    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "rb")
        self.total_size = os.path.getsize(path)
        self.f.seek(16 * SECTOR)
        pvd = self.f.read(SECTOR)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise ValueError("Kein ISO-9660-Primary-Volume-Descriptor in Sektor 16 "
                             "(CSO/komprimiert? Dann zuerst nach ISO umwandeln.)")
        self.system_id = pvd[8:40].decode("ascii", "replace").strip()
        self.volume_id = pvd[40:72].decode("ascii", "replace").strip()
        self.volume_blocks = struct.unpack_from("<I", pvd, 80)[0]
        self.block_size = struct.unpack_from("<H", pvd, 128)[0]
        self.app_id = pvd[574:702].decode("ascii", "replace").strip()
        self.creation = pvd[813:830].decode("ascii", "replace").strip()
        root = pvd[156:190]
        self.root_lba, self.root_size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        self.entries: list[IsoFile] = []
        self._walk("", self.root_lba, self.root_size, set())
        self.entries.sort(key=lambda e: e.path)
        self.by_path = {e.path.upper(): e for e in self.entries}

    def _walk(self, prefix: str, lba: int, size: int, seen: set):
        if lba in seen:
            return
        seen.add(lba)
        self.f.seek(lba * SECTOR)
        data = self.f.read(size)
        pos = 0
        while pos < len(data):
            rec_len = data[pos]
            if rec_len == 0:  # Sektor-Padding
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = data[pos:pos + rec_len]
            pos += rec_len
            e_lba, e_size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            flags, name_len = rec[25], rec[32]
            name = rec[33:33 + name_len]
            if name in (b"\x00", b"\x01"):
                continue
            text = name.decode("ascii", "replace").split(";")[0]
            if text.endswith("."):
                text = text[:-1]
            path = prefix + text
            is_dir = bool(flags & 2)
            self.entries.append(IsoFile(path, e_lba, e_size, is_dir))
            if is_dir:
                self._walk(path + "/", e_lba, e_size, seen)

    def find(self, path: str) -> IsoFile | None:
        return self.by_path.get(path.replace("\\", "/").strip("/").upper())

    def read(self, entry: IsoFile, length: int | None = None, offset: int = 0) -> bytes:
        n = entry.size - offset if length is None else min(length, entry.size - offset)
        self.f.seek(entry.lba * SECTOR + offset)
        return self.f.read(max(n, 0))

    def is_all_zero(self, entry: IsoFile) -> bool:
        off = 0
        while off < entry.size:
            if self.read(entry, CHUNK, off).strip(b"\x00"):
                return False
            off += CHUNK
        return True

    def extract(self, entry: IsoFile, dest: str):
        os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
        with open(dest, "wb") as out:
            off = 0
            while off < entry.size:
                out.write(self.read(entry, CHUNK, off))
                off += CHUNK


# ---------------------------------------------------------------- Prüfsummen

def checksums(path: str, progress: bool = True) -> dict:
    crc, md5, sha1 = 0, hashlib.md5(), hashlib.sha1()
    total, done = os.path.getsize(path), 0
    with open(path, "rb") as f:
        while True:
            buf = f.read(CHUNK)
            if not buf:
                break
            crc = zlib.crc32(buf, crc)
            md5.update(buf)
            sha1.update(buf)
            done += len(buf)
            if progress:
                print(f"\rPrüfsummen: {done * 100 // max(total, 1)} %", end="", file=sys.stderr)
    if progress:
        print("\r" + " " * 24 + "\r", end="", file=sys.stderr)
    return {"size": total, "crc32": f"{crc & 0xFFFFFFFF:08x}", "md5": md5.hexdigest(), "sha1": sha1.hexdigest()}


# ---------------------------------------------------------------- PARAM.SFO

def parse_sfo(data: bytes) -> dict:
    if data[:4] != b"\x00PSF":
        raise ValueError("PARAM.SFO: falsche Magic")
    _ver, key_off, data_off, count = struct.unpack_from("<IIII", data, 4)
    out = {}
    for i in range(count):
        k_off, fmt, length, _max_len, d_off = struct.unpack_from("<HHIII", data, 20 + i * 16)
        key = data[key_off + k_off:data.index(b"\x00", key_off + k_off)].decode("ascii")
        raw = data[data_off + d_off:data_off + d_off + length]
        if fmt == 0x0404:
            out[key] = struct.unpack_from("<I", raw)[0]
        else:
            out[key] = raw.rstrip(b"\x00").decode("utf-8", "replace")
    return out


# ---------------------------------------------------------------- Programmdateien

def classify_executable(iso: Iso, entry: IsoFile) -> dict:
    """Nur am Header erkennen, was es ist."""
    info = {"path": entry.path, "size": entry.size}
    head = iso.read(entry, 0x60)
    if entry.size == 0:
        info["kind"] = "leer (0 Bytes)"
    elif head[:4] == b"~PSP":
        info["kind"] = "verschlüsselt (~PSP-Header)"
        info["module_name"] = head[0x0A:0x26].split(b"\x00")[0].decode("ascii", "replace")
        info["elf_size"] = struct.unpack_from("<I", head, 0x28)[0]
        info["psp_size"] = struct.unpack_from("<I", head, 0x2C)[0]
        info["compressed"] = bool(struct.unpack_from("<H", head, 0x06)[0] & 1)
    elif head[:4] == b"\x7fELF":
        e_type, e_machine = struct.unpack_from("<HH", head, 16)
        e_entry, e_phoff, e_shoff = struct.unpack_from("<III", head, 24)
        e_phnum = struct.unpack_from("<H", head, 44)[0]
        e_shnum = struct.unpack_from("<H", head, 48)[0]
        kinds = {2: "ET_EXEC (feste Adresse)", 0xFFA0: "PRX (0xFFA0, relozierbar)"}
        info["kind"] = "unverschlüsseltes ELF"
        info["elf_type"] = kinds.get(e_type, hex(e_type))
        info["machine"] = "MIPS" if e_machine == 8 else str(e_machine)
        info["entry"] = f"0x{e_entry:08X}"
        info["program_headers"], info["section_headers"] = e_phnum, e_shnum
        # alles innerhalb der Datei? sonst abgeschnitten
        end = max(e_shoff + e_shnum * 40, e_phoff + e_phnum * 32)
        for i in range(e_phnum):
            ph = iso.read(entry, 32, e_phoff + i * 32)
            if len(ph) == 32:
                p_offset, _va, _pa, p_filesz = struct.unpack_from("<IIII", ph, 4)
                end = max(end, p_offset + p_filesz)
        info["complete"] = end <= entry.size
        if iso.is_all_zero(entry):
            info["kind"] = "nur Nullbytes"
    elif iso.is_all_zero(entry):
        info["kind"] = "nur Nullbytes"
    else:
        info["kind"] = "unbekannt (Magic " + head[:4].hex() + ")"
    return info


# ---------------------------------------------------------------- Ausgabe

def human(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return str(n)


def dir_sizes(iso: Iso) -> dict:
    sizes: dict[str, list[int]] = {}
    for e in iso.entries:
        if e.is_dir:
            continue
        parts = e.path.split("/")
        for i in range(1, len(parts)):
            s = sizes.setdefault("/".join(parts[:i]), [0, 0])
            s[0] += e.size
            s[1] += 1
    return sizes


def collect(iso: Iso, do_hash: bool) -> dict:
    result = {
        "file": os.path.basename(iso.path),
        "volume": {"system_id": iso.system_id, "volume_id": iso.volume_id, "application_id": iso.app_id,
                   "creation": iso.creation, "blocks": iso.volume_blocks, "block_size": iso.block_size,
                   "size_matches_file": iso.volume_blocks * iso.block_size == iso.total_size},
    }
    result["checksums"] = checksums(iso.path) if do_hash else {"size": iso.total_size}
    sfo = iso.find("PSP_GAME/PARAM.SFO")
    result["param_sfo"] = parse_sfo(iso.read(sfo)) if sfo else None
    umd = iso.find("UMD_DATA.BIN")
    result["umd_data"] = iso.read(umd).decode("ascii", "replace").strip() if umd else None
    exes = []
    for e in iso.entries:
        low = e.path.lower()
        if not e.is_dir and (low.endswith((".prx", ".elf")) or os.path.basename(low) in ("boot.bin", "eboot.bin")):
            exes.append(classify_executable(iso, e))
    result["executables"] = exes
    result["files"] = [{"path": e.path, "size": e.size, "lba": e.lba} for e in iso.entries if not e.is_dir]
    return result


def print_text(iso: Iso, r: dict, depth: int, top: int):
    c, v = r["checksums"], r["volume"]
    print(f"Datei:      {r['file']}")
    print(f"Größe:      {c['size']} Bytes ({human(c['size'])})")
    if "crc32" in c:
        print(f"CRC32:      {c['crc32']}\nMD5:        {c['md5']}\nSHA-1:      {c['sha1']}")
    print(f"Volume:     {v['volume_id']!r}, System {v['system_id']!r}, erstellt {v['creation']}")
    print(f"Blöcke:     {v['blocks']} x {v['block_size']}"
          f" ({'passt zur Dateigröße' if v['size_matches_file'] else 'WEICHT von der Dateigröße ab'})")
    if r["umd_data"]:
        print(f"UMD_DATA:   {r['umd_data']}")
    if r["param_sfo"]:
        print("\nPARAM.SFO:")
        for k, val in r["param_sfo"].items():
            print(f"  {k:<18} {val}")
    print("\nProgrammdateien:")
    for x in r["executables"]:
        extra = ", ".join(f"{k}={val}" for k, val in x.items() if k not in ("path", "size", "kind"))
        print(f"  {x['path']:<44} {x['size']:>10}  {x['kind']}" + (f"  [{extra}]" if extra else ""))
    sizes = dir_sizes(iso)
    print(f"\nDateibaum (Tiefe {depth}):")
    for e in iso.entries:
        level = e.path.count("/") + 1
        if level > depth:
            continue
        indent = "  " * level
        if e.is_dir:
            s = sizes.get(e.path, [0, 0])
            print(f"{indent}{e.path.split('/')[-1] + '/':<32} {human(s[0]):>12}  ({s[1]} Dateien)")
        else:
            print(f"{indent}{e.path.split('/')[-1]:<32} {human(e.size):>12}")
    files = [e for e in iso.entries if not e.is_dir]
    print(f"\nGrößte Dateien (von {len(files)}):")
    for e in sorted(files, key=lambda e: -e.size)[:top]:
        print(f"  {human(e.size):>12}  {e.path}")
    exts: dict[str, list[int]] = {}
    for e in files:
        s = exts.setdefault(os.path.splitext(e.path)[1].lower() or "(ohne)", [0, 0])
        s[0] += 1
        s[1] += e.size
    print("\nDateitypen:")
    for ext, (n, size) in sorted(exts.items(), key=lambda kv: -kv[1][1]):
        print(f"  {ext:<10} {n:>6} Dateien  {human(size):>12}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Prüfsummen und Inhalt einer PSP-ISO anzeigen (entschlüsselt nichts).")
    ap.add_argument("iso", help="Pfad zur ISO, z.B. game/diva1st.iso")
    ap.add_argument("--json", action="store_true", help="maschinenlesbare Ausgabe (enthält die vollständige Dateiliste)")
    ap.add_argument("--no-hash", action="store_true", help="Prüfsummen überspringen (schneller)")
    ap.add_argument("--depth", type=int, default=2, help="Tiefe des Dateibaums in der Textausgabe (Standard 2)")
    ap.add_argument("--top", type=int, default=15, help="Anzahl der größten Dateien (Standard 15)")
    ap.add_argument("--list", action="store_true", help="alle Dateien mit Größe auflisten")
    ap.add_argument("--extract", nargs=2, metavar=("DATEI_IN_ISO", "ZIEL"),
                    help="eine einzelne Datei unverändert extrahieren (Ziel nur unter game/ oder generated/ ablegen!)")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    try:
        iso = Iso(args.iso)
    except (OSError, ValueError) as err:
        print(f"Fehler: {err}", file=sys.stderr)
        return 1

    if args.extract:
        entry = iso.find(args.extract[0])
        if entry is None or entry.is_dir:
            print(f"Fehler: {args.extract[0]} nicht in der ISO gefunden", file=sys.stderr)
            return 1
        iso.extract(entry, args.extract[1])
        print(f"{entry.path} ({entry.size} Bytes) -> {args.extract[1]}")
        return 0
    if args.list:
        for e in iso.entries:
            if not e.is_dir:
                print(f"{e.size:>12}  {e.path}")
        return 0

    result = collect(iso, not args.no_hash)
    if args.json:
        json.dump(result, sys.stdout, indent=1, ensure_ascii=False)
        print()
    else:
        print_text(iso, result, args.depth, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
