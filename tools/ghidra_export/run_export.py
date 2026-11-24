#!/usr/bin/env python3
"""Ghidra-Headless-Analyse + JSON-Export

    py tools/ghidra_export/run_export.py game/decrypted/EBOOT.BIN
    py tools/ghidra_export/run_export.py game/decrypted/EBOOT.BIN --out generated/analysis/diva1st --reuse

Ergebnis generated/analysis/<name>/*.json (functions, imports, exports, relocations, instr_histogram, jumptables, program),
stammt aus dem Spiel: nicht einchecken.

Umgebungsvariablen:
    GHIDRA_HOME  (Standard C:\\Tools\\ghidra_12.2_PUBLIC)
    JAVA_HOME    (Standard C:\\Program Files\\Eclipse Adoptium\\jdk-25.0.5.8-hotspot; Ghidra 12.2 braucht JDK 25)
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEFAULT_GHIDRA = r"C:\Tools\ghidra_12.2_PUBLIC"
DEFAULT_JAVA = r"C:\Program Files\Eclipse Adoptium\jdk-25.0.5.8-hotspot"


def main() -> int:
    ap = argparse.ArgumentParser(description="Ghidra-Headless-Export für ein PSP-Programm (ELF/PRX, unverschlüsselt).")
    ap.add_argument("binary", help="z.B. game/decrypted/EBOOT.BIN")
    ap.add_argument("--name", default="diva1st", help="Name für Projekt und Ausgabeordner (Standard diva1st)")
    ap.add_argument("--out", help="Ausgabeordner (Standard generated/analysis/<name>)")
    ap.add_argument("--reuse", action="store_true",
                    help="vorhandenes Ghidra-Projekt weiterverwenden und nur den Export neu ausführen")
    ap.add_argument("--script", default="ExportAnalysis.java", help="Post-Skript (Standard ExportAnalysis.java)")
    ap.add_argument("script_args", nargs="*", help="weitere Argumente für das Skript (nach dem Ausgabeordner)")
    args = ap.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        print(f"Fehler: {binary} nicht gefunden", file=sys.stderr)
        return 1
    with open(binary, "rb") as f:
        magic = f.read(4)
    if magic == b"~PSP":
        print("Fehler: Datei ist noch verschlüsselt (~PSP). Zuerst auf der PSP entschlüsseln (S3).", file=sys.stderr)
        return 1
    if magic != b"\x7fELF":
        print(f"Fehler: kein ELF (Magic {magic.hex()})", file=sys.stderr)
        return 1

    ghidra = Path(os.environ.get("GHIDRA_HOME", DEFAULT_GHIDRA))
    headless = ghidra / "support" / "analyzeHeadless.bat"
    if not headless.is_file():
        print(f"Fehler: {headless} nicht gefunden (GHIDRA_HOME setzen)", file=sys.stderr)
        return 1
    env = dict(os.environ)
    env.setdefault("JAVA_HOME", DEFAULT_JAVA)
    if "JAVA_HOME" not in os.environ:
        env["JAVA_HOME"] = DEFAULT_JAVA

    out = Path(args.out).resolve() if args.out else ROOT / "generated" / "analysis" / args.name
    proj_dir = ROOT / "generated" / "ghidra"
    proj_dir.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    log = out / "ghidra_headless.log"

    cmd = [str(headless), str(proj_dir), args.name]
    if args.reuse:
        cmd += ["-process", binary.name, "-noanalysis"]
    else:
        cmd += ["-import", str(binary), "-overwrite"]
    cmd += ["-scriptPath", str(HERE), "-postScript", args.script, str(out), *args.script_args,
            "-log", str(log)]
    print("Starte:", " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    return subprocess.call(cmd, env=env)


if __name__ == "__main__":
    sys.exit(main())
