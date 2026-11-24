# at3_standalone

ATRAC3/ATRAC3plus-Decoder aus FFmpeg, von PPSSPP als eigenständiger Code herausgelöst
(`ext/at3_standalone` im PPSSPP-Repository, Stand Commit `1a6d0c1ff0b878d4dd82886dcc819fa9f5741c1f`).
Unverändert übernommen. Lizenz: **GNU LGPL 2.1 oder später** (siehe Kopf jeder Datei).

Verwendet von `runtime/hle/mpeg_video.cpp` für den Ton der PSMF-Videos (`sceMpegAtracDecode`).
Die Ersatz-Header in `shim/` sind eigene, kleine Stellvertreter für PPSSPP-Header (nicht aus PPSSPP).
