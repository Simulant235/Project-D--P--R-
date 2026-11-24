#!/usr/bin/env python3
"""HLE-Stubs aus runtime/hle/nids.csv; ohne --force nur fehlende Bibliotheken, vorhandene Dateien bleiben
"""
import argparse
import csv
from pathlib import Path

NO_NET = 0x80410B00
RET = {"sceNet": NO_NET, "sceNetAdhoc": NO_NET, "sceNetAdhocctl": NO_NET,
       "sceNetAdhocMatching": NO_NET, "sceWlanDrv": 0}
HEADER = ("// Automatisch erzeugt von tools/gen_hle_stubs.py aus runtime/hle/nids.csv. "
          "Implementierte Funktionen hier löschen.\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="runtime/hle/nids.csv")
    ap.add_argument("--out", default="runtime/hle/stubs")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    libs = {}
    with open(a.csv, encoding="utf-8", newline="") as fh:
        for r in csv.DictReader(fh):
            libs.setdefault(r["library"], []).append(r)

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for lib, rows in libs.items():
        f = out / f"{lib}.cpp"
        if f.exists() and not a.force:
            print("uebersprungen:", f.name)
            continue
        ret = RET.get(lib, 0)
        rets = "0x%08X" % ret if ret else "0"
        body = "".join(
            f'PSP_HLE_STUB({lib}, {r["nid"]}, "{r["name"] or "?"}", {rets})\n' for r in rows)
        text = (HEADER + '\n#include "psp_hle.h"\n\nextern "C" {\n\n' + body
                + '\n} // extern "C"\n')
        f.write_text(text, encoding="utf-8", newline="\n")
        print("geschrieben:", f.name, len(rows))


if __name__ == "__main__":
    main()
