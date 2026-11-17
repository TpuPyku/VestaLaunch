@echo off
rem Builds and runs test_preflight.exe (preflight.h on the PC, VS2005 x86) in obj\; exit code: the failed checks.
rem "build": only builds (the mutator runs it).
setlocal
if "%VS8%"=="" set VS8=C:\Program Files (x86)\Microsoft Visual Studio 8
set VC=%VS8%\VC
set PATH=%VS8%\Common7\IDE;%VC%\bin;%PATH%
cd /d "%~dp0"
if not exist obj mkdir obj
"%VC%\bin\cl.exe" /nologo /O2 /W3 /MT /TP test_preflight.cpp /Foobj\ /Fdobj\ /Feobj\test_preflight.exe ^
  /DUNICODE /D_UNICODE /DWIN32 /D_CRT_SECURE_NO_WARNINGS /I"%VC%\include" /I"%VC%\PlatformSDK\Include" ^
  /link /LIBPATH:"%VC%\lib" /LIBPATH:"%VC%\PlatformSDK\Lib" kernel32.lib >obj\build_preflight.log || (type obj\build_preflight.log & exit /b 1)
if "%1"=="build" exit /b 0
obj\test_preflight.exe
