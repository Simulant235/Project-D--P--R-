"""SDL3 (Visual C++) nach third_party/SDL3 laden, nicht im Git
py tools/fetch_sdl3.py, danach CMake neu konfigurieren; ohne SDL3 nur PS4/PS5 (HID) und Xbox
"""
import io, os, shutil, sys, urllib.request, zipfile

VERSION = "3.4.20"
URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/SDL3-devel-{VERSION}-VC.zip"
DEST = os.path.join(os.path.dirname(__file__), "..", "third_party", "SDL3")

def main():
    if os.path.exists(os.path.join(DEST, "include", "SDL3", "SDL.h")):
        print("SDL3 ist schon da:", os.path.abspath(DEST)); return 0
    print("lade", URL)
    data = urllib.request.urlopen(URL).read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        root = f"SDL3-{VERSION}/"
        for n in z.namelist():
            if not n.startswith(root) or n.endswith("/"): continue
            rel = n[len(root):]
            if not (rel.startswith("include/") or rel.startswith("lib/x64/") or rel in ("LICENSE.txt", "README.md")): continue
            out = os.path.join(DEST, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(n) as src, open(out, "wb") as dst: shutil.copyfileobj(src, dst)
    print("fertig:", os.path.abspath(DEST))
    return 0

if __name__ == "__main__":
    sys.exit(main())
