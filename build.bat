@echo off
rem Builds The Worst Map Mod for the Greeneye Loader: bin\loader\unlock-map-markers.dll + mod.json
rem (install with deploy.py while the game is closed, or hot reload it with
rem EnshroudedMods\tools\hot-reload.ps1 unlock-map-markers while it runs).
setlocal

rem The shared hooking headers (greeneye.h, jumppatch.h): a copy ships in src\. Point
rem GREENEYE_COMMON at another folder to build against that one instead.
if not defined GREENEYE_COMMON set "GREENEYE_COMMON=%~dp0src"
if not exist "%GREENEYE_COMMON%\greeneye.h" (
    echo greeneye.h not found in %GREENEYE_COMMON%
    exit /b 1
)

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo Failed to initialize VS2022 x64 build environment
    exit /b 1
)

cd /d "%~dp0"
if not exist bin\loader mkdir bin\loader
if not exist build mkdir build

echo Compiling unlock-map-markers.dll...
cl /nologo /std:c++20 /O2 /EHsc /MT /LD /W4 /I "%GREENEYE_COMMON%" ^
    src\main.cpp src\map_key_panel.cpp src\scan.cpp ^
    /Fo:build\ ^
    /Fe:bin\loader\unlock-map-markers.dll ^
    /link /INCREMENTAL:NO /IMPLIB:build\unlock-map-markers.lib user32.lib psapi.lib
if errorlevel 1 exit /b 1
copy /y mod.json bin\loader\mod.json >nul
if errorlevel 1 exit /b 1

echo Compiling test_patches.exe...
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 ^
    tests\test_patches.cpp ^
    /Fo:build\test_patches.obj ^
    /Fe:bin\test_patches.exe
if errorlevel 1 exit /b 1

echo Running offline binary patch verification...
bin\test_patches.exe
if errorlevel 1 exit /b 1

echo Built bin\loader\unlock-map-markers.dll for Greeneye Loader API 2
