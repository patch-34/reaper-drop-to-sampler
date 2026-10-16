@echo off
setlocal EnableExtensions

set "DLL_NAME=reaper_drop_to_sampler.dll"

rem %~dp0 ends with backslash; trailing \ before " breaks quoting on Windows
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD_DIR=%ROOT%\build"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

echo === Configure ===
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ "%ROOT%"
if errorlevel 1 goto cmake_fail

echo.
echo === Build ===
ninja
if errorlevel 1 goto build_fail

if not exist "%BUILD_DIR%\%DLL_NAME%" goto dll_missing

echo.
echo === Done ===
echo DLL: %BUILD_DIR%\%DLL_NAME%
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

:all_done
endlocal
