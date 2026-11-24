# Changelog

## [0.1.0-alpha.1]

Erste Alpha.

- Recompiler MIPS/Allegrex → C, mit FPU/VFPU, Sprungtabellen, Delay Slots (T10-T12)
- Runtime: Speicher, Threads, Systemfunktionen, Dateien aus der ISO, Spielstände (T20, T24)
- Software-Grafik (CPU), Ton über waveOut, Videos mit Ton (T21, T22)
- Controller über SDL3, Tastatur W/A/S/D oder I/J/K/L (T32)
- F3 Leistungsanzeige (FPS, tiefste der letzten 60 s, 1% low, Latenz, CPU, RAM), F4 speichert die letzten 15 s
- Launcher (T41): baut das Spiel aus der eigenen ISO, verschlüsselte ISOs über EBOOT.BIN aus PPSSPP,
  Fehler melden, Deutsch/Englisch, Deinstallieren
- Absturz- und Hängererkennung, Log wird laufend gesichert
- Setup ohne Admin (T41), Updater (T87), Alpha läuft bis 14.12.2026 (T90)
- Hinweis zum Datenschutz beim ersten Start; Lizenztexte von llvm-mingw, MinGW-w64 und CMake im Installationsordner
