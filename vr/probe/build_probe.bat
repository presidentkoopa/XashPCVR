@echo off
REM Build the 32-bit OpenXR capability probe.
REM 32-bit on purpose: the engine builds 32-bit (GoldSrc mod DLLs are 32-bit),
REM so we must probe the runtime that answers a 32-bit process.

setlocal

set VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community
set XRSDK=E:\OpenXR-SDK
set XRBUILD=%XRSDK%\build32\src\loader\Release

call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if errorlevel 1 (
  echo FAILED to set up 32-bit MSVC environment
  exit /b 1
)

cl /nologo /W3 /O2 /MD ^
   /I "%XRSDK%\include" ^
   "%~dp0xr_probe.c" ^
   /Fe:"%~dp0xr_probe.exe" ^
   /Fo:"%~dp0xr_probe.obj" ^
   /link "%XRBUILD%\openxr_loader.lib"

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

REM loader dll must sit next to the exe
copy /Y "%XRBUILD%\openxr_loader.dll" "%~dp0" >nul

echo BUILD OK: %~dp0xr_probe.exe
endlocal
