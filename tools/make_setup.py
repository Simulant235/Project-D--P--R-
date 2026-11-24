"""make_setup.py: baut DIVA-Setup-<version>.exe und das Update-Paket

Nach einem Release-Bau (Ziele diva_launcher diva_setup diva_recomp):
    py tools/make_setup.py                 -> dist/DIVA-Setup-<v>.exe, DIVA-Update-<v>.zip (+ .sha256)
    py tools/make_setup.py --update-tools  -> Update enthält auch tools/ (nur bei neuem llvm-mingw/CMake)

Paket = Installationsordner: DIVA Launcher.exe, bin/diva_recomp.exe, src/ (Quellen zum Bauen), tools/llvm-mingw, tools/cmake
Keine Spieldateien, nichts aus game/ oder generated/.

Setup-Exe = stub + ZIP + b"DIVAPKG1" + Größe (8 Byte LE), siehe installer/setup.cpp
"""
import argparse
import glob
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# was in src/ kommt (aus Git, auch neue Dateien)
SRC_PREFIXES = ('CMakeLists.txt', 'VERSION', 'LICENSE', 'cmake/', 'runtime/', 'recompiler/', 'games/', 'third_party/')
SRC_SKIP = ('runtime/tests/', 'recompiler/tests/')   # BUILD_TESTING=OFF


def git_files():
    out = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard'], cwd=ROOT, capture_output=True, text=True,
                         encoding='utf-8', check=True).stdout
    files = []
    for f in out.splitlines():
        if not f.startswith(SRC_PREFIXES) or f.startswith(SRC_SKIP):
            continue
        if not os.path.isfile(os.path.join(ROOT, f)):   # gelöscht, noch im Index
            continue
        files.append(f)
    return files


def add_tree(entries, src_dir, arc_dir, skip_dirs=()):
    for base, dirs, names in os.walk(src_dir):
        dirs[:] = [d for d in dirs if d not in skip_dirs]
        for n in names:
            full = os.path.join(base, n)
            rel = os.path.relpath(full, src_dir).replace('\\', '/')
            entries.append((full, arc_dir + '/' + rel))


def write_zip(path, entries):
    seen = set()
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for i, (full, arc) in enumerate(entries):
            if arc in seen:
                continue
            seen.add(arc)
            z.write(full, arc)
            if i % 2000 == 0:
                print(f'  {i}/{len(entries)} {arc}', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', default=os.path.join(ROOT, 'build'), help='Build-Ordner (Visual Studio, Release)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist'))
    ap.add_argument('--cmake-dir', help='CMake-Installation (Ordner mit bin/ und share/); Standard: die cmake.exe im PATH')
    ap.add_argument('--update-tools', action='store_true', help='tools/ auch ins Update-Paket')
    a = ap.parse_args()

    version = open(os.path.join(ROOT, 'VERSION'), encoding='utf-8').read().strip()
    launcher = os.path.join(a.build, 'installer', 'Release', 'DIVA Launcher.exe')
    stub = os.path.join(a.build, 'installer', 'Release', 'DIVA-Setup-stub.exe')
    recomp = os.path.join(a.build, 'recompiler', 'Release', 'diva_recomp.exe')
    for f in (launcher, stub, recomp):
        if not os.path.isfile(f):
            sys.exit(f'fehlt: {f}\nZuerst bauen: cmake --build build --config Release --target diva_launcher diva_setup diva_recomp')

    tc = sorted(glob.glob(os.path.join(ROOT, 'tools', 'llvm-mingw-*')))
    if not tc or not os.path.isfile(os.path.join(tc[-1], 'bin', 'clang.exe')):
        sys.exit('llvm-mingw fehlt in tools/ (llvm-mingw-*-ucrt-x86_64)')
    cmake_dir = a.cmake_dir
    if not cmake_dir:
        exe = shutil.which('cmake')
        if not exe:
            sys.exit('cmake nicht gefunden (--cmake-dir angeben)')
        cmake_dir = os.path.dirname(os.path.dirname(os.path.realpath(exe)))
    share = sorted(glob.glob(os.path.join(cmake_dir, 'share', 'cmake-*')))
    if not os.path.isfile(os.path.join(cmake_dir, 'bin', 'cmake.exe')) or not share:
        sys.exit(f'keine CMake-Installation in {cmake_dir}')
    sdl = os.path.join(ROOT, 'third_party', 'SDL3')
    if not os.path.isfile(os.path.join(sdl, 'lib', 'x64', 'SDL3.dll')):
        print('Warnung: third_party/SDL3 fehlt (py tools/fetch_sdl3.py), Controller dann nur per HID/XInput')

    # Programm
    prog = [(launcher, 'DIVA Launcher.exe'), (recomp, 'bin/diva_recomp.exe')]
    for f in git_files():
        prog.append((os.path.join(ROOT, f), 'src/' + f))
    if os.path.isdir(sdl):
        add_tree(prog, sdl, 'src/third_party/SDL3')
    # Werkzeuge (nur Setup bzw. --update-tools)
    tools = []
    # nur x64: ARM/32-Bit-Sysroots und Python raus (~260 MB)
    add_tree(tools, tc[-1], 'tools/llvm-mingw', skip_dirs=('python', 'i686-w64-mingw32', 'armv7-w64-mingw32', 'aarch64-w64-mingw32',
                                                            'arm64ec-w64-mingw32', 'busybox'))
    tools.append((os.path.join(cmake_dir, 'bin', 'cmake.exe'), 'tools/cmake/bin/cmake.exe'))
    add_tree(tools, share[-1], 'tools/cmake/share/' + os.path.basename(share[-1]), skip_dirs=('Help',))   # keine Doku
    lic = os.path.join(cmake_dir, 'doc', 'cmake')   # Lizenztexte müssen mit
    if os.path.isdir(lic):
        add_tree(tools, lic, 'tools/cmake/doc/cmake', skip_dirs=('html',))
        tools[:] = [t for t in tools if not t[0].endswith('.qch')]

    os.makedirs(a.out, exist_ok=True)
    pkg = os.path.join(a.out, f'package-{version}.zip')
    print(f'Paket (Setup): {len(prog)} Programmdateien + {len(tools)} Werkzeugdateien')
    write_zip(pkg, prog + tools)
    setup = os.path.join(a.out, f'DIVA-Setup-{version}.exe')
    size = os.path.getsize(pkg)
    with open(setup, 'wb') as o:
        with open(stub, 'rb') as f:
            shutil.copyfileobj(f, o)
        with open(pkg, 'rb') as f:
            shutil.copyfileobj(f, o, 16 << 20)
        o.write(b'DIVAPKG1' + struct.pack('<Q', size))
    os.remove(pkg)
    print(f'-> {setup} ({os.path.getsize(setup) >> 20} MB)')

    upd = os.path.join(a.out, f'DIVA-Update-{version}.zip')
    print('Update-Paket' + (' (mit tools/)' if a.update_tools else ''))
    write_zip(upd, prog + (tools if a.update_tools else []))
    h = hashlib.sha256()
    with open(upd, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    with open(upd + '.sha256', 'w', encoding='ascii', newline='\n') as f:
        f.write(f'{h.hexdigest()}  {os.path.basename(upd)}\n')
    print(f'-> {upd} ({os.path.getsize(upd) >> 20} MB), sha256 {h.hexdigest()}')


if __name__ == '__main__':
    main()
