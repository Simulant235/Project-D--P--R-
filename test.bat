@echo off
rem baut alles, Tests, optional Spiel
rem   test.bat            bauen + Tests
rem   test.bat recomp     zusaetzlich das Spiel neu recompilieren (nach Aenderungen am Recompiler/diva1st.toml)
rem   test.bat run        zusaetzlich 1st 150 Sekunden starten (Log: generated\run.log)
rem   test.bat recomp run beides
setlocal
cd /d "%~dp0"

set DO_RECOMP=0
set DO_RUN=0
for %%A in (%*) do (
  if /I "%%A"=="recomp" set DO_RECOMP=1
  if /I "%%A"=="run" set DO_RUN=1
)

echo === 1/4 Konfigurieren ===
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 >nul || goto :fail

if "%DO_RECOMP%"=="1" (
  echo === Recompilieren ===
  if not exist game\decrypted mkdir game\decrypted
  py tools\iso_info.py game\diva1st.iso --extract PSP_GAME/SYSDIR/EBOOT.BIN game\decrypted\EBOOT.BIN || goto :fail
  cmake --build build --config Release --target diva_recomp -- -v:m -nologo >nul || goto :fail
  build\recompiler\Release\diva_recomp.exe --game games/diva1st.toml --eboot game\decrypted\EBOOT.BIN --out generated\diva1st || goto :fail
)

echo === 2/4 Bauen ===
cmake --build build --config Release -- -v:m -nologo || goto :fail

echo === 3/4 Tests ===
ctest --test-dir build -C Release --output-on-failure || goto :fail

echo === 4/4 Spiel ===
if "%DO_RUN%"=="1" (
  echo Startet 1st fuer 150 Sekunden ohne Fenster. Log: generated\run.log
  if not exist generated mkdir generated
  powershell -NoProfile -Command "$p = Start-Process -FilePath 'build\runtime\Release\diva1st.exe' -ArgumentList '--iso game\diva1st.iso --log generated\run.log --dump-threads 20' -PassThru -NoNewWindow; if (-not $p.WaitForExit(150000)) { $p.Kill(); Write-Host 'nach 150 s beendet' } else { Write-Host ('beendet mit Exit-Code ' + $p.ExitCode) }"
  findstr /C:"|E|" generated\run.log
  echo Letzte Zeilen mit STATS/DUMP:
  findstr /C:"psmf:" /C:"exit 'soundThread'" generated\run.log
) else (
  echo uebersprungen. Mit "test.bat run" startest du das Spiel.
)
echo.
echo FERTIG: alles gebaut, alle Tests bestanden.
exit /b 0

:fail
echo.
echo FEHLER: siehe Ausgabe oben.
exit /b 1
