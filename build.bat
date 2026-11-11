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
rem The version in the log: mod.json's
for /f "tokens=2 delims=:, " %%v in ('findstr /c:"\"version\"" mod.json') do set "UMM_VER=%%~v"
cl /nologo /std:c++20 /O2 /EHsc /MT /LD /W4 /utf-8 /DUMM_VERSION=\"%UMM_VER%\" /I "%GREENEYE_COMMON%" ^
    src\main.cpp src\map_key_panel.cpp src\scan.cpp src\lang.cpp ^
    /Fo:build\ ^
    /Fe:bin\loader\unlock-map-markers.dll ^
    /link /INCREMENTAL:NO /IMPLIB:build\unlock-map-markers.lib user32.lib psapi.lib
if errorlevel 1 exit /b 1
copy /y mod.json bin\loader\mod.json >nul
if errorlevel 1 exit /b 1
copy /y data\veins.bin bin\loader\veins.bin >nul
copy /y data\ground.bin bin\loader\ground.bin >nul
copy /y data\halls.bin bin\loader\halls.bin >nul
if errorlevel 1 exit /b 1
rem The vaults' and spires' traps, doors and switches (tools\resources\make_traps.py)
copy /y data\traps.bin bin\loader\traps.bin >nul
if errorlevel 1 exit /b 1
rem The roads and surface water (tools\resources\make_resources.py, make_water.py)
copy /y data\roads.bin bin\loader\roads.bin >nul
if errorlevel 1 exit /b 1
copy /y data\water.bin bin\loader\water.bin >nul
if errorlevel 1 exit /b 1
rem The map desk (tools\resources\make_desk.py)
if not exist bin\loader\desk mkdir bin\loader\desk
copy /y data\desk\* bin\loader\desk\ >nul
if errorlevel 1 exit /b 1
if not exist bin\loader\desk\hi mkdir bin\loader\desk\hi
copy /y data\desk\hi\* bin\loader\desk\hi\ >nul
if errorlevel 1 exit /b 1
if not exist bin\loader\desk\mid mkdir bin\loader\desk\mid
copy /y data\desk\mid\* bin\loader\desk\mid\ >nul
rem the survivor sheets in each language (tools\resources\desk_langs.py)
if exist data\desk\lang xcopy /e /i /y /q data\desk\lang bin\loader\desk\lang >nul
rem the props the mod draws itself (tools\resources\desk_props.py)
if exist data\desk\props xcopy /e /i /y /q data\desk\props bin\loader\desk\props >nul
if errorlevel 1 exit /b 1
rem The Hollow Halls' floor textures for the HD dungeon maps (tools\resources\make_halls_hd.py textures)
if not exist bin\loader\hd mkdir bin\loader\hd
copy /y data\hd\* bin\loader\hd\ >nul
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

cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_player_markers.cpp /Fo:build\test_player_markers.obj /Fe:bin\test_player_markers.exe
if errorlevel 1 exit /b 1

cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_filter_notice.cpp /Fo:build\test_filter_notice.obj /Fe:bin\test_filter_notice.exe
if errorlevel 1 exit /b 1
bin\test_filter_notice.exe
if errorlevel 1 exit /b 1
bin\test_player_markers.exe
if errorlevel 1 exit /b 1

cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tests\test_hd_pool.cpp /Fo:build\test_hd_pool.obj /Fe:bin\test_hd_pool.exe
if errorlevel 1 exit /b 1
bin\test_hd_pool.exe
if errorlevel 1 exit /b 1

echo Built bin\loader\unlock-map-markers.dll for Greeneye Loader API 2
