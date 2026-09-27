@echo off
rem ===========================================================================
rem  record_laria.cmd
rem ---------------------------------------------------------------------------
rem  Starts the Laria visualizer as a clean game window (no editor panels) at a
rem  fixed resolution, ready to be captured by OBS (Window Capture) or the Xbox
rem  Game Bar (Win+Alt+R). Then Run the scenario LIVE from the ScenarioEditor:
rem  a live run sends the directed camera shots and the deck crew; a .disrec
rem  replay does not.
rem
rem  Usage:  record_laria.cmd [width] [height]      (default 1920 1080)
rem          set LARIA_DIR / UE_EDITOR first to override the paths below.
rem ===========================================================================
setlocal

set "RESX=%~1"
set "RESY=%~2"
if "%RESX%"=="" set "RESX=1920"
if "%RESY%"=="" set "RESY=1080"

if "%LARIA_DIR%"=="" set "LARIA_DIR=%USERPROFILE%\Documents\Unreal Projects\laria-sim-visualizer"
if "%UE_EDITOR%"=="" set "UE_EDITOR=C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
set "UPROJECT=%LARIA_DIR%\LariaSimVisualizer.uproject"

if not exist "%UE_EDITOR%" (
    echo Unreal Editor not found: "%UE_EDITOR%"
    echo Set UE_EDITOR to your UnrealEditor.exe and try again.
    exit /b 1
)
if not exist "%UPROJECT%" (
    echo Laria project not found: "%UPROJECT%"
    echo Set LARIA_DIR to the laria-sim-visualizer folder and try again.
    exit /b 1
)

echo Starting Laria at %RESX%x%RESY% ...
echo   1. Point OBS Window Capture at the Laria window (or click it and press Win+Alt+R).
echo   2. Start recording.
echo   3. In the ScenarioEditor, Run the scenario (live, not a .disrec replay).
echo   4. Stop recording after the explosion; close the Laria window when done.
start "Laria" "%UE_EDITOR%" "%UPROJECT%" -game -windowed -ResX=%RESX% -ResY=%RESY% -log
endlocal
