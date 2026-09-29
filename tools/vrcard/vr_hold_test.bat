@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >NUL 2>&1
cd /d "%~dp0"
cl /nologo /W3 /I"E:\XashWork\XashFWGS\engine\client\vr" /Fe:vr_hold_test.exe vr_hold_test.c "E:\XashWork\XashFWGS\engine\client\vr\vr_hold.c" >NUL
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
"%~dp0vr_hold_test.exe"
