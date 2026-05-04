@echo off
setlocal enabledelayedexpansion

for %%I in ("%~dp0.") do set "REPO_ROOT=%%~fI"
pushd "%REPO_ROOT%" >nul

set "CONFIG=Debug"
set "MODE=run"
set "RUN_ARGS="

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="debug" (set "CONFIG=Debug" & shift & goto parse_args)
if /i "%~1"=="release" (set "CONFIG=Release" & shift & goto parse_args)
if /i "%~1"=="configure" (set "MODE=configure" & shift & goto parse_args)
if /i "%~1"=="build" (set "MODE=build" & shift & goto parse_args)
if /i "%~1"=="run" (set "MODE=run" & shift & goto parse_args)
if /i "%~1"=="clean" (set "MODE=clean" & shift & goto parse_args)
set "RUN_ARGS=!RUN_ARGS! %1"
shift
goto parse_args

:args_done
set "BUILD_DIR=builds\cmake"
set "EXE_NAME=glint_qem_studio_gui.exe"
set "CMAKE_TARGET=glint_qem_studio_gui"
set "EXE="

echo.
echo ========================================
echo  QEM Studio Build and Run Script
echo ========================================
echo Mode:   %MODE%
echo Config: %CONFIG%
echo Repo:   %REPO_ROOT%
echo.

if /i "%MODE%"=="clean" goto do_clean
if /i "%MODE%"=="configure" goto do_configure
if /i "%MODE%"=="build" goto do_build
if /i "%MODE%"=="run" goto do_run

echo ERROR: Unknown mode %MODE%
exit /b 1

:do_clean
if exist "%BUILD_DIR%" (
  echo Removing %BUILD_DIR% ...
  rmdir /S /Q "%BUILD_DIR%"
) else (
  echo Build directory does not exist: %BUILD_DIR%
)
exit /b 0

:do_configure
echo [1/1] Configuring QEM Studio ...
cmake -S apps/qem_simplifier -B "%BUILD_DIR%"
if errorlevel 1 exit /b 1
exit /b 0

:do_build
call :configure_if_needed
if errorlevel 1 exit /b 1
echo [1/1] Building %CONFIG% target %CMAKE_TARGET% ...
cmake --build "%BUILD_DIR%" --config %CONFIG% --target %CMAKE_TARGET% -j
if errorlevel 1 exit /b 1
exit /b 0

:do_run
call :build_target
if errorlevel 1 exit /b 1
call :resolve_exe %CONFIG%
if errorlevel 1 exit /b 1
echo Launching: %EXE%
"%EXE%" %RUN_ARGS%
exit /b %ERRORLEVEL%

:configure_if_needed
if not exist "%BUILD_DIR%\CMakeCache.txt" (
  echo [1/2] Configuring QEM Studio ...
  cmake -S . -B "%BUILD_DIR%"
  if errorlevel 1 exit /b 1
)
exit /b 0

:build_target
call :configure_if_needed
if errorlevel 1 exit /b 1
echo [2/2] Building %CONFIG% target %CMAKE_TARGET% ...
cmake --build "%BUILD_DIR%" --config %CONFIG% --target %CMAKE_TARGET% -j
if errorlevel 1 exit /b 1
exit /b 0

:resolve_exe
set "RESOLVE_CONFIG=%~1"
for %%P in (
  "%BUILD_DIR%\%RESOLVE_CONFIG%\%EXE_NAME%"
  "%BUILD_DIR%\Release\%EXE_NAME%"
  "%BUILD_DIR%\RelWithDebInfo\%EXE_NAME%"
  "%BUILD_DIR%\MinSizeRel\%EXE_NAME%"
  "%BUILD_DIR%\Debug\%EXE_NAME%"
  "%BUILD_DIR%\%EXE_NAME%"
) do (
  if exist "%%~P" (
    set "EXE=%%~fP"
    exit /b 0
  )
)
echo ERROR: %EXE_NAME% not found under %BUILD_DIR%
exit /b 1
