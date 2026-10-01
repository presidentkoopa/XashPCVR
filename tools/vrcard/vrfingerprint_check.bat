@echo off
rem Build vrfingerprint_check and run it over every retail-bound card, pairing
rem each with the model it claims. Answers one question: will the engine bind it?
rem
rem vcvars once, then run the exe per card - calling a vcvars-wrapping batch
rem file in a loop overflows PATH and cmd stops dead part way through.
setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "HL=%HL_ROOT%"
if "%HL%"=="" set "HL=D:\SteamLibrary\steamapps\common\Half-Life"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >NUL 2>&1
cd /d "%HERE%"

if not exist "%TEMP%\fpobj\" md "%TEMP%\fpobj"
cl /nologo /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:"%TEMP%\vrfp.exe" /Fo"%TEMP%\fpobj\\" vrfingerprint_check.c >NUL
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )

set BIND=0
set NOBIND=0

echo == cards\hd  (valve_hd, and _bshift against bshift_hd) ==
for %%C in ("%HERE%cards\hd\*.card") do (
	set "N=%%~nC"
	set "DIR=valve_hd"
	set "MDL=!N!"
	if "!N:~-7!"=="_bshift" (
		set "DIR=bshift_hd"
		set "MDL=!N:_bshift=!"
	)
	"%TEMP%\vrfp.exe" "%HL%\!DIR!\models\!MDL!.mdl" "%%~fC" > "%TEMP%\fp.txt" 2>&1
	if errorlevel 1 ( set /a NOBIND+=1 & echo   NO BIND  %%~nxC & type "%TEMP%\fp.txt" ) else ( set /a BIND+=1 & echo   binds    %%~nxC )
)

echo.
echo == cards\gearbox  (retail Opposing Force) ==
for %%C in ("%HERE%cards\gearbox\*.card") do (
	"%TEMP%\vrfp.exe" "%HL%\gearbox\models\%%~nC.mdl" "%%~fC" > "%TEMP%\fp.txt" 2>&1
	if errorlevel 1 ( set /a NOBIND+=1 & echo   NO BIND  %%~nxC & type "%TEMP%\fp.txt" ) else ( set /a BIND+=1 & echo   binds    %%~nxC )
)

echo.
echo binds: !BIND!   will not bind: !NOBIND!
rd /s /q "%TEMP%\fpobj" >NUL 2>&1
del /q "%TEMP%\vrfp.exe" "%TEMP%\fp.txt" >NUL 2>&1
if !NOBIND! GTR 0 exit /b 1
exit /b 0
