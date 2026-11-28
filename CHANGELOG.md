# Changelog

## [0.2.1-alpha.1]

- Absturz beim Schließen des Spiels behoben (Raster-Threads beim Beenden)
- Fehlerbericht: optionale Beschreibung, ohne Benutzernamen in Pfaden, mit Absturzcode, vorherigem Log und erkannter Fassung, bleibt unter 10 MB

## [0.2.0-alpha.1]

- Einstellungen aufgebaut wie im Hauptprojekt: Renderer, Auflösung, Bilder pro Sekunde und xBRZ schon sichtbar, aber noch "(bald)"
- Design des Launchers wählbar (wie Windows, hell, dunkel)
- Schalter "Unbekannte Fassung trotzdem bauen (auf eigenes Risiko)": nur nach Eintippen von RISIKO / RISK, Recompiler mit --ignore-hash

## [0.1.3-alpha.1]

- Spiel startete nicht (Code 12), wenn vorher eine andere Fassung angefangen wurde: alte EBOOT aus PPSSPP wird jetzt entfernt, abgebrochene Installationen hinterlassen keine Reste

## [0.1.2-alpha.1]

- Japanische Originalfassung von 1st (ohne Fan-Patch) wird jetzt erkannt und gebaut

## [0.1.1-alpha.1]

- Setup: Sprache oben rechts wählbar (DE/EN), gilt auch gleich für Launcher und Spiel

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
