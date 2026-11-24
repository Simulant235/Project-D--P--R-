#!/usr/bin/env python3
"""NID-Tabelle der Importe erzeugen
    py tools/nid_table.py generated/analysis/diva1st/imports.json \
        --ppsspp third_party/ppsspp --out runtime/hle/nids.csv
Namen nur aus den PPSSPP-Funktionstabellen (Core/HLE/*.cpp); ohne Quellcode leer (source=unbekannt)
"""
import argparse
import csv
import json
import re
import sys
from pathlib import Path

ENTRY = re.compile(r'\{\s*0[xX]([0-9A-Fa-f]{8})\s*,[^"\n]*"([^"]+)"')


def scan_ppsspp(root: Path):
    """Liefert {nid: {dateiname: [namen]}}."""
    table = {}
    hle = root / "Core" / "HLE"
    if not hle.is_dir():
        return None
    for f in sorted(hle.glob("*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        for m in ENTRY.finditer(text):
            nid = int(m.group(1), 16)
            names = table.setdefault(nid, {}).setdefault(f.name, [])
            if m.group(2) not in names:
                names.append(m.group(2))
    return table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("imports")
    ap.add_argument("--ppsspp", default="third_party/ppsspp")
    ap.add_argument("--out", default="runtime/hle/nids.csv")
    a = ap.parse_args()

    libs = json.loads(Path(a.imports).read_text(encoding="utf-8"))
    table = scan_ppsspp(Path(a.ppsspp))
    if table is None:
        print("WARNUNG: PPSSPP-Quellcode fehlt, Namen offen", file=sys.stderr)

    rows = []
    for lib in libs:
        libname = lib["library"]
        for fn in sorted(lib["functions"], key=lambda f: int(f["nid"], 16)):
            nid = int(fn["nid"], 16)
            name, source = "", "unbekannt"
            found = (table or {}).get(nid)
            if found:
                # Datei mit passendem Bibliotheksnamen bevorzugen
                pref = [n for f, ns in found.items() if libname.lower() in f.lower() for n in ns]
                allnames = []
                for ns in found.values():
                    for n in ns:
                        if n not in allnames:
                            allnames.append(n)
                if len(pref) == 1:
                    name, source = pref[0], "ppsspp"
                elif len(allnames) == 1:
                    name, source = allnames[0], "ppsspp"
                else:
                    name, source = "|".join(allnames), "ppsspp-mehrdeutig"
            rows.append([libname, "0x%08X" % nid, name, fn["call_sites"], source])

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="") as fh:
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(["library", "nid", "name", "stub_addr_count", "source"])
        w.writerows(rows)
    named = sum(1 for r in rows if r[4] != "unbekannt")
    print(f"{len(rows)} Zeilen, {len({r[0] for r in rows})} Bibliotheken, benannt: {named}, unbekannt: {len(rows)-named}")


if __name__ == "__main__":
    main()
