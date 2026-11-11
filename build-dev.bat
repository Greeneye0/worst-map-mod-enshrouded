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
rem The version in the log: mod.json's
for /f "tokens=2 delims=:, " %%v in ('findstr /c:"\"version\"" mod.json') do set "UMM_VER=%%~v"
cl /nologo /std:c++20 /O2 /EHsc /MT /LD /W4 /utf-8 /DUMM_VERSION=\"%UMM_VER%\" /DUMM_DEV_BRIDGE /DGREENEYE_INPUT_TEST /I "%GREENEYE_COMMON%" ^
    src\main.cpp src\map_key_panel.cpp src\scan.cpp src\lang.cpp ^
    /Fo:build-dev\ ^
    /Fe:bin-dev\loader\unlock-map-markers.dll ^
    /link /INCREMENTAL:NO /IMPLIB:build-dev\unlock-map-markers.lib user32.lib psapi.lib
if errorlevel 1 exit /b 1
copy /y mod.json bin-dev\loader\mod.json >nul
if errorlevel 1 exit /b 1
copy /y data\veins.bin bin-dev\loader\veins.bin >nul
copy /y data\ground.bin bin-dev\loader\ground.bin >nul
if errorlevel 1 exit /b 1
copy /y data\roads.bin bin-dev\loader\roads.bin >nul
if errorlevel 1 exit /b 1
copy /y data\water.bin bin-dev\loader\water.bin >nul
if errorlevel 1 exit /b 1
rem The map desk (tools\resources\make_desk.py)
if not exist bin-dev\loader\desk mkdir bin-dev\loader\desk
copy /y data\desk\* bin-dev\loader\desk\ >nul
if errorlevel 1 exit /b 1
if not exist bin-dev\loader\desk\hi mkdir bin-dev\loader\desk\hi
copy /y data\desk\hi\* bin-dev\loader\desk\hi\ >nul
if errorlevel 1 exit /b 1
if not exist bin-dev\loader\desk\mid mkdir bin-dev\loader\desk\mid
copy /y data\desk\mid\* bin-dev\loader\desk\mid\ >nul
rem the survivor sheets in each language (tools\resources\desk_langs.py)
if exist data\desk\lang xcopy /e /i /y /q data\desk\lang bin-dev\loader\desk\lang >nul
rem the props the mod draws itself (tools\resources\desk_props.py)
if exist data\desk\props xcopy /e /i /y /q data\desk\props bin-dev\loader\desk\props >nul
if errorlevel 1 exit /b 1
echo Built bin-dev\loader\unlock-map-markers.dll (DEV - bridge exports included)
