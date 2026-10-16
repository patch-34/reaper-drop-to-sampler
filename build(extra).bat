@echo off
setlocal EnableExtensions

set "DLL_NAME=reaper_drop_to_sampler.dll"
set "PLUGIN_DIR=%APPDATA%\REAPER\UserPlugins"
set "REAPER_EXE=C:\Program Files\REAPER (x64)\reaper.exe"

rem %~dp0 ends with backslash; trailing \ before " breaks quoting on Windows
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD_DIR=%ROOT%\build"

echo === Closing REAPER if running ===
tasklist /FI "IMAGENAME eq reaper.exe" 2>NUL | find /I "reaper.exe" >NUL
if errorlevel 1 goto reaper_not_running
taskkill /IM reaper.exe >NUL 2>&1
echo Waiting for REAPER to exit...
:wait_reaper
timeout /T 1 /NOBREAK >NUL
tasklist /FI "IMAGENAME eq reaper.exe" 2>NUL | find /I "reaper.exe" >NUL
if not errorlevel 1 goto wait_reaper
echo REAPER closed.
goto reaper_done_close
:reaper_not_running
echo REAPER is not running.
:reaper_done_close

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

echo.
echo === Configure ===
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ "%ROOT%"
if errorlevel 1 goto cmake_fail

echo.
echo === Build ===
ninja
if errorlevel 1 goto build_fail

if not exist "%BUILD_DIR%\%DLL_NAME%" goto dll_missing

echo.
echo === Copy to UserPlugins ===
if not exist "%PLUGIN_DIR%" mkdir "%PLUGIN_DIR%"
copy /Y "%BUILD_DIR%\%DLL_NAME%" "%PLUGIN_DIR%\%DLL_NAME%"
if errorlevel 1 goto copy_fail
echo Copied to: %PLUGIN_DIR%\%DLL_NAME%

echo.
echo === Start REAPER ===
if not exist "%REAPER_EXE%" goto reaper_missing
start "" "%REAPER_EXE%"
echo Started: %REAPER_EXE%
goto all_done

:cmake_fail
echo CMake configure failed
pause
exit /b 1

:build_fail
echo Build failed
pause
exit /b 1

:dll_missing
echo DLL not found: %BUILD_DIR%\%DLL_NAME%
pause
exit /b 1

:copy_fail
echo Copy failed - is the DLL locked?
pause
exit /b 1

:reaper_missing
echo REAPER not found:
echo %REAPER_EXE%
echo Edit REAPER_EXE at the top of build(extra).bat if needed.
pause
exit /b 1

:all_done
echo.
echo === Done ===
endlocal
