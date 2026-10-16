@echo off
setlocal
set "DLL=%~dp0build\reaper_drop_to_sampler.dll"
if not exist "%DLL%" (
  echo DLL not found: %DLL%
  echo Build first with build.bat
  pause
  exit /b 1
)

echo === File ===
dir "%DLL%"

echo.
echo === DLL dependencies (objdump) ===
where objdump >nul 2>&1
if errorlevel 1 (
  echo objdump not in PATH. Use MSYS2 UCRT64 or Dependencies.exe
  pause
  exit /b 1
)

objdump -p "%DLL%" | findstr /I "DLL Name"

echo.
findstr /I "libwinpthread libgcc libstdc++" "%TEMP%\dts_deps.txt" >nul 2>&1
objdump -p "%DLL%" | findstr /I "libwinpthread libgcc libstdc++" > "%TEMP%\dts_bad.txt"
for %%A in ("%TEMP%\dts_bad.txt") do set SIZE=%%~zA
if not "%SIZE%"=="0" (
  echo.
  echo *** BAD: MinGW runtime still dynamic: ***
  type "%TEMP%\dts_bad.txt"
  echo *** Rebuild failed static link - do not distribute this DLL. ***
) else (
  echo.
  echo OK: no libwinpthread / libgcc / libstdc++ in dependency list.
)

echo.
echo Note: api-ms-win-crt-*.dll is normal on Win10/11 (Universal CRT).
echo.
pause
