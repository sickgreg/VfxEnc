@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
for %%I in ("%ROOT%.") do set "ROOT=%%~fI"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "BUILD_DIR=%ROOT%\build"
set "DEPS_DIR=%ROOT%\deps"
set "DIST_DIR=%ROOT%\dist"
set "DIST_DEPS=%DIST_DIR%\deps"

set "MPV_DEV_DIR=%DEPS_DIR%\mpv-dev"
set "MPV_RT_DIR=%DEPS_DIR%\mpv-rt"
set "FFMPEG_DIR=%DEPS_DIR%\ffmpeg"
set "PAUSE_ON_FAIL=1"

rem Ensure previous instance isn't locking the exe
taskkill /im VfxEnc.exe /f /t >nul 2>&1

set "CLEAN_ONLY=0"
for %%A in (%*) do (
  if /I "%%~A"=="clean" set "CLEAN_ONLY=1"
  if /I "%%~A"=="/clean" set "CLEAN_ONLY=1"
)

if "%CLEAN_ONLY%"=="1" (
  del /f /q "%ROOT%\*.obj" >nul 2>&1
  del /f /q "%ROOT%\*.pdb" >nul 2>&1
  del /f /q "%ROOT%\*.ilk" >nul 2>&1
  del /f /q "%ROOT%\*.exp" >nul 2>&1
  del /f /q "%ROOT%\*.lib" >nul 2>&1
  del /f /q "%ROOT%\*.log" >nul 2>&1
  del /f /q "%ROOT%\combined_shaders_*.glsl" >nul 2>&1
  if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%" >nul 2>&1
  if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%" >nul 2>&1
  if exist "%DEPS_DIR%" rmdir /s /q "%DEPS_DIR%" >nul 2>&1
  echo Clean complete.
  exit /b 0
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%" >nul
if not exist "%DEPS_DIR%" mkdir "%DEPS_DIR%" >nul
del /f /q "%ROOT%\*.obj" >nul 2>&1
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%" >nul 2>&1
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%" >nul
if not exist "%DIST_DEPS%" mkdir "%DIST_DEPS%" >nul

call :ensure_deps
if errorlevel 1 goto :fail

rem Copy sample shaders
if not exist "%DIST_DIR%\glsl" mkdir "%DIST_DIR%\glsl" >nul
if exist "%ROOT%\glsl" (
  xcopy /y /e /i "%ROOT%\glsl" "%DIST_DIR%\glsl" >nul
)

call :setup_vs
if errorlevel 1 goto :fail

call :build_app
if errorlevel 1 goto :fail

echo.
echo Build complete.
echo Output: %DIST_DIR%\VfxEnc.exe
echo Deps:   %DIST_DEPS%
exit /b 0

:ensure_deps
echo [deps] Ensuring mpv + ffmpeg...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\build_deps.ps1" ^
  -DepsDir "%DEPS_DIR%" ^
  -MpvDevDir "%MPV_DEV_DIR%" ^
  -MpvRtDir "%MPV_RT_DIR%" ^
  -FfmpegDir "%FFMPEG_DIR%"
if errorlevel 1 exit /b 1

rem Copy runtime deps to dist
set "MPV_DEV_DLL="
for /r "%MPV_DEV_DIR%" %%F in (libmpv-2.dll) do (
  set "MPV_DEV_DLL=%%~fF"
  goto :mpvdevdllfound
)
:mpvdevdllfound
if not defined MPV_DEV_DLL (
  echo ERROR: libmpv-2.dll not found in %MPV_DEV_DIR%
  exit /b 1
)

set "FFMPEG_EXE="
for /f "delims=" %%F in ('dir /b /s "%FFMPEG_DIR%\\ffmpeg.exe" 2^>nul') do (
  set "FFMPEG_EXE=%%F"
  goto :fffound
)
:fffound
if not defined FFMPEG_EXE (
  echo ERROR: ffmpeg.exe not found in %FFMPEG_DIR%
  exit /b 1
)
if not exist "%FFMPEG_EXE%" (
  echo ERROR: ffmpeg.exe path invalid: %FFMPEG_EXE%
  exit /b 1
)

if not exist "%DIST_DEPS%" mkdir "%DIST_DEPS%" >nul

copy /y "%MPV_DEV_DLL%" "%DIST_DEPS%\mpv-2.dll" >nul

for /r "%MPV_RT_DIR%" %%F in (*.dll) do (
  copy /y "%%~fF" "%DIST_DEPS%" >nul
)
copy /y "%FFMPEG_EXE%" "%DIST_DEPS%\ffmpeg.exe" >nul
if errorlevel 1 (
  echo ERROR: failed to copy ffmpeg.exe to %DIST_DEPS%
  exit /b 1
)

exit /b 0

:setup_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
)
if not defined VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
if not defined VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
if not defined VSINSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise" set "VSINSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"

if not defined VSINSTALL (
  echo ERROR: Visual Studio with VC tools not found.
  exit /b 1
)

if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" (
  call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
) else if exist "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" (
  call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=amd64
) else (
  echo ERROR: Could not find vcvarsall.bat or VsDevCmd.bat in %VSINSTALL%
  exit /b 1
)
if errorlevel 1 exit /b 1

exit /b 0

:build_app
echo [build] Compiling...
set "MPV_LIB_DIR="
if exist "%MPV_DEV_DIR%\lib\mpv.lib" set "MPV_LIB_DIR=%MPV_DEV_DIR%\lib"
if not defined MPV_LIB_DIR (
  for /f "delims=" %%F in ('dir /b /s "%MPV_DEV_DIR%\mpv.lib" 2^>nul') do (
    set "MPV_LIB_DIR=%%~dpF"
    goto :mpvlibfound
  )
)
:mpvlibfound
if not defined MPV_LIB_DIR set "MPV_LIB_DIR=%MPV_DEV_DIR%\lib"
if not exist "%MPV_LIB_DIR%\mpv.lib" (
  call :make_mpv_lib
  if errorlevel 1 exit /b 1
  set "MPV_LIB_DIR=%MPV_DEV_DIR%\lib"
)

set "MPV_INCLUDE="
if exist "%MPV_DEV_DIR%\include\mpv\client.h" set "MPV_INCLUDE=%MPV_DEV_DIR%\include"
if not defined MPV_INCLUDE (
  for /r "%MPV_DEV_DIR%" %%F in (client.h) do (
    for %%D in ("%%~dpF..") do set "MPV_INCLUDE=%%~fD"
    goto :mpvincfound
  )
)
:mpvincfound
if not defined MPV_INCLUDE (
  echo ERROR: mpv\client.h not found in %MPV_DEV_DIR%
  exit /b 1
)

echo [build] MPV_LIB_DIR=%MPV_LIB_DIR%
echo [build] MPV_INCLUDE=%MPV_INCLUDE%

cl /nologo /std:c++17 /EHsc /O2 /MD /DNDEBUG /DUNICODE /D_UNICODE ^
  /I"%MPV_INCLUDE%" ^
  /Fe:"%DIST_DIR%\VfxEnc.exe" "%ROOT%\VfxEnc.cpp" ^
  /link /SUBSYSTEM:WINDOWS /DELAYLOAD:mpv-2.dll /LIBPATH:"%MPV_LIB_DIR%" mpv.lib delayimp.lib User32.lib Gdi32.lib Comdlg32.lib Shell32.lib
if errorlevel 1 exit /b 1

exit /b 0

:make_mpv_lib
echo [build] Generating mpv.lib from libmpv-2.dll...
set "MPV_DEV_DLL="
for /r "%MPV_DEV_DIR%" %%F in (libmpv-2.dll) do (
  set "MPV_DEV_DLL=%%~fF"
  goto :mpvdevdllfound2
)
:mpvdevdllfound2
if not defined MPV_DEV_DLL (
  echo ERROR: libmpv-2.dll not found to generate mpv.lib
  exit /b 1
)

if not exist "%MPV_DEV_DIR%\lib" mkdir "%MPV_DEV_DIR%\lib" >nul
set "MPV_DEF=%BUILD_DIR%\mpv.def"
if exist "%MPV_DEF%" del /f /q "%MPV_DEF%" >nul
echo LIBRARY mpv-2.dll> "%MPV_DEF%"
echo EXPORTS>> "%MPV_DEF%"
where dumpbin >nul 2>&1
if errorlevel 1 (
  echo ERROR: dumpbin.exe not found in PATH. Is the VC environment initialized?
  exit /b 1
)
where lib >nul 2>&1
if errorlevel 1 (
  echo ERROR: lib.exe not found in PATH. Is the VC environment initialized?
  exit /b 1
)
for /f "tokens=4" %%A in ('dumpbin /exports "%MPV_DEV_DLL%" ^| findstr /R /C:"^[ ]*[0-9][0-9]*"') do (
  if not "%%A"=="" echo %%A>> "%MPV_DEF%"
)

lib /nologo /def:"%MPV_DEF%" /out:"%MPV_DEV_DIR%\lib\mpv.lib" /machine:x64
if errorlevel 1 exit /b 1
if not exist "%MPV_DEV_DIR%\lib\mpv.lib" (
  echo ERROR: mpv.lib was not created.
  exit /b 1
)

exit /b 0

:fail
echo.
echo Build failed.
if defined PAUSE_ON_FAIL pause
exit /b 1
