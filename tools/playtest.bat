@echo off
rem ====================================================================
rem One command from a working tree to a gun in your hand.
rem
rem   tools\playtest.bat                build everything, deploy, launch
rem   tools\playtest.bat -tests         ...and run the headless suites first
rem   tools\playtest.bat -build-only    build and deploy, do not launch
rem   tools\playtest.bat -launch-only   launch what is already deployed
rem
rem FOUR things have to arrive together or the VR weapon path does nothing,
rem and each one being stale fails silently and differently:
rem
rem   xash.dll            an old engine never negotiates the hand channel on
rem                       loopback, so the server never steps a weapon
rem   valve\dlls\hl.dll   Valve's own means none of our weapon code exists
rem   valve\cl_dlls\
rem     client.dll        where prediction lives
rem   <gamedir>\vr\cards\ without these every weapon takes the vanilla path BY
rem                       DESIGN, so the simulator is inert and nothing looks
rem                       wrong
rem
rem This builds and deploys all four, so they cannot drift apart. 32-bit
rem throughout: a 64-bit engine cannot load Half-Life's 32-bit hl.dll, which is
rem what XashVR64 is for and why it cannot play the game.
rem ====================================================================
setlocal enabledelayedexpansion

set "ENGINE=E:\XashWork\XashFWGS"
set "HLSDK=E:\XashWork\hlsdk-portable"
set "PLAY=E:\XashWork\XashVR"
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

rem ====================================================================
rem EXIT CODES: EVERY FAILURE PATH GOES THROUGH :fail.
rem
rem It used to `exit /b 1` from inside the parenthesised `if %DO_BUILD%==1 (
rem ... )` block, and that reported SUCCESS. The block printed FAILED, stopped
rem before deploying, and handed back 0 - so anything reading the exit code
rem was told a broken build had shipped. Caught by breaking the game DLL on
rem purpose and watching this return 0 with "error C2059" on screen.
rem
rem Same family as the two lies vr_test_all.bat already carries warnings
rem about: a runner that cannot report its own failure is worse than no
rem runner, because it converts a loud failure into a silent one.
rem ====================================================================
set DO_BUILD=1
set DO_LAUNCH=1
set DO_TESTS=0

for %%A in (%*) do (
	if /i "%%A"=="-tests"       set DO_TESTS=1
	if /i "%%A"=="-build-only"  set DO_LAUNCH=0
	if /i "%%A"=="-launch-only" set DO_BUILD=0
)

if %DO_TESTS%==1 (
	echo [1/5] headless suites ...
	call "%HLSDK%\dlls\vr_test_all.bat" > "%TEMP%\pt_tests.txt" 2>&1
	if errorlevel 1 (
		echo       FAILED - not deploying code that does not pass its own tests
		type "%TEMP%\pt_tests.txt" | findstr /i /c:"FAIL" /c:"BUILD FAILED"
		goto :fail
	)
	for /f %%N in ('type "%TEMP%\pt_tests.txt" ^| find /c "  ok   "') do echo       %%N cases pass
)

if %DO_BUILD%==1 (
	echo [2/5] engine, 32-bit ...
	pushd "%ENGINE%"
	python waf build > "%TEMP%\pt_engine.txt" 2>&1
	if errorlevel 1 (
		echo       FAILED
		type "%TEMP%\pt_engine.txt" | findstr /i /c:"error" /c:"Error"
		popd & goto :fail
	)
	popd
	echo       ok

	echo [3/5] game DLLs, 32-bit Release ...
	"%CMAKE%" --build "%HLSDK%\build" --config Release > "%TEMP%\pt_dlls.txt" 2>&1
	if errorlevel 1 (
		echo       FAILED
		type "%TEMP%\pt_dlls.txt" | findstr /i /c:"error"
		goto :fail
	)
	echo       ok

	echo [4/5] deploy engine, DLLs and cards to %PLAY% ...
	python "%ENGINE%\tools\deploy.py" "%PLAY%" --arch 32
	if errorlevel 1 ( echo       FAILED & goto :fail )
)

if %DO_LAUNCH%==0 (
	echo.
	echo built and deployed; not launching.
	exit /b 0
)

echo [5/5] launching ...
echo.
echo ------------------------------------------------------------------
echo  Carded weapons pick up the simulator. Anything without a card
echo  behaves exactly as it always has, which is by design, not a fault.
echo.
echo  Worth trying, in rough order of how much they exercise:
echo    shotgun    pump it by hand; it loads one shell at a time
echo    pistol     rack the slide; the magazine is CARVED out of the
echo               mesh at load, so its coming out is mesh surgery and
echo               card binding both working end to end
echo    revolver   swing the cylinder out and LET GO - it should stay
echo               out while you fetch a loader. It would not before.
echo    MP5        should fire automatic. It drew semi before.
echo    grenade    the pin ring should stay out once pulled
echo.
echo  Not carded in this install: egon, gauss, hornet gun.
echo  Weapon controls (safety, selector, magazine release) do NOT work
echo  yet - they need the grip solver, which is not built.
echo ------------------------------------------------------------------
echo.

rem gl_check_errors is a DEVELOPER DIAGNOSTIC, not a fault indicator, and
rem run.bat launches with -dev 2 which switches it on. It calls glGetError at
rem the top of entity drawing and prints whatever is pending - and GL errors
rem are sticky, so one upstream call that a driver dislikes prints once per
rem entity per frame forever. That is how one run produced 7,508 identical
rem lines and drowned the log that actually matters.
rem
rem Off by default so the game is usable and the log is readable. To chase a
rem rendering fault, pass -glcheck and read what it says - but note the file
rem and line it prints are where the error was DETECTED, not where it was
rem caused.
set "GLCHK=+gl_check_errors 0"
for %%A in (%*) do if /i "%%A"=="-glcheck" set "GLCHK=+gl_check_errors 1"

call "%PLAY%\run.bat" %GLCHK% %*

rem ...and a clean run must not fall THROUGH into the failure label below,
rem which is the other half of getting exit codes right.
exit /b 0

rem ---- the only failure exit, and it is at TOP LEVEL on purpose --------
rem `exit /b 1` from inside a parenthesised block reported 0. Everything that
rem fails jumps here instead.
:fail
echo.
echo BUILD OR TESTS FAILED - nothing was deployed and nothing was launched.
exit /b 1
