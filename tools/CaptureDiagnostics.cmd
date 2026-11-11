@echo off
setlocal
cd /d "%~dp0"
set "GAME="
set "GAMES=0"
for %%G in (*Recomp.exe) do (
    set "GAME=%%~fG"
    set /a GAMES+=1 >nul
)
if not "%GAMES%"=="1" (
    echo Keep this script beside exactly one game executable.
    exit /b 2
)
set "RECOMP_INPUT_REPORT=%~dp0controller-input.jsonl"
set "RECOMP_INPUT_REPORT_MS=15000"
echo The launcher will record controller discovery and input for 15 seconds.
echo Select your controller and press then release its buttons during capture.
echo Close the launcher normally when finished.
"%GAME%" --launcher > "%~dp0launcher-diagnostics.log" 2>&1
set "GAME_EXIT=%ERRORLEVEL%"
echo Exit code: %GAME_EXIT%>> "%~dp0launcher-diagnostics.log"
echo Reports: launcher-diagnostics.log and controller-input.jsonl
exit /b %GAME_EXIT%
