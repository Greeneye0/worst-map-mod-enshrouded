@echo off
rem DEV build of The Worst Map Mod: build.bat's DLL plus the MapNativeControl / MapNativeFilters
rem exports for the local QuickDeposit dev bridge (UMM_DEV_BRIDGE). Never ship this one.
rem Hot reload it with: EnshroudedMods\tools\hot-reload.ps1 unlock-map-markers -Dev
setlocal
if not defined GREENEYE_COMMON set "GREENEYE_COMMON=%~dp0src"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist bin-dev\loader mkdir bin-dev\loader
if not exist build-dev mkdir build-dev
cl /nologo /std:c++20 /O2 /EHsc /MT /LD /W4 /utf-8 /DUMM_DEV_BRIDGE /I "%GREENEYE_COMMON%" ^
    src\main.cpp src\map_key_panel.cpp src\scan.cpp src\lang.cpp ^
    /Fo:build-dev\ ^
    /Fe:bin-dev\loader\unlock-map-markers.dll ^
    /link /INCREMENTAL:NO /IMPLIB:build-dev\unlock-map-markers.lib user32.lib psapi.lib
if errorlevel 1 exit /b 1
copy /y mod.json bin-dev\loader\mod.json >nul
if errorlevel 1 exit /b 1
copy /y data\veins.bin bin-dev\loader\veins.bin >nul
if errorlevel 1 exit /b 1
echo Built bin-dev\loader\unlock-map-markers.dll (DEV - bridge exports included)
