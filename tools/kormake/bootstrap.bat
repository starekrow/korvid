@echo off
rem Builds kormake from source with MSVC: the vendored Lua, the embedded scripts, and the
rem host. Output: tools\kormake\bin\windows\kormake.exe. Run from anywhere. Uses the cl on
rem PATH (a Developer Command Prompt) if there is one; otherwise finds the newest Visual
rem Studio or Build Tools with the C++ tools through vswhere and sets it up for this run.
setlocal
set "here=%~dp0"
set "here=%here:~0,-1%"
set "out=%here%\bin\windows"
set "tmp=%here%\bin\.build"
if not exist "%out%" mkdir "%out%" || goto fail
if not exist "%tmp%" mkdir "%tmp%" || goto fail

where /q cl.exe
if not errorlevel 1 goto have_cl
set "arch=x64"
set "vccomp=Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
  set "arch=arm64"
  set "vccomp=Microsoft.VisualStudio.Component.VC.Tools.ARM64"
)
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" goto no_vs
"%vswhere%" -latest -prerelease -products * -requires %vccomp% -property installationPath > "%tmp%\vsdir.txt"
set "vsdir="
set /p vsdir=<"%tmp%\vsdir.txt"
if not defined vsdir goto no_vs
echo kormake: using %vsdir%
rem vcvarsall's helpers look for vswhere on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%vsdir%\VC\Auxiliary\Build\vcvarsall.bat" %arch% >nul || goto fail
where /q cl.exe
if errorlevel 1 goto no_vs

:have_cl
echo kormake: building for windows with cl
set "cflags=/nologo /O2 /std:c17 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS"

cl %cflags% /Fo"%tmp%\\" /Fe"%tmp%\embed.exe" "%here%\src\embed.c" >"%tmp%\embed.log" || (type "%tmp%\embed.log" & goto fail)

set "scripts="
for %%f in ("%here%\scripts\*.lua") do call :add_script "%%~ff"
"%tmp%\embed.exe" "%tmp%\scripts.inc" %scripts% || goto fail

set "luasrc="
for %%f in ("%here%\vendor\lua\*.c") do (
  if /i not "%%~nxf"=="lua.c" if /i not "%%~nxf"=="luac.c" call :add_lua "%%~ff"
)
cl %cflags% /I"%here%\vendor\lua" /I"%tmp%" /Fo"%tmp%\\" /Fe"%out%\kormake.exe" "%here%\src\kormake.c" "%here%\src\os_win32.c" %luasrc% >"%tmp%\kormake.log" || (type "%tmp%\kormake.log" & goto fail)
echo kormake: %out%\kormake.exe
endlocal
exit /b 0

:add_script
set scripts=%scripts% %1
goto :eof

:add_lua
set luasrc=%luasrc% %1
goto :eof

:no_vs
echo kormake: no cl.exe on PATH and no Visual Studio with the C++ tools ^(%vccomp%^) found. 1>&2
echo kormake: install Visual Studio or the Build Tools with "Desktop development with C++", 1>&2
echo kormake: or run this from a Developer Command Prompt. 1>&2
endlocal
exit /b 1

:fail
echo kormake: bootstrap failed 1>&2
endlocal
exit /b 1
