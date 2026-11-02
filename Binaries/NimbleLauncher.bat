@echo off
rem NimbleLauncher: File > New Project without building the editor.
rem
rem     Engine\Binaries\NimbleLauncher.bat [--no-warmup]
rem
rem Configures Engine\Build\launcher the first time (which also prebuilds the
rem third-party libraries into Engine\Intermediate\ThirdParty, shared with every
rem game's tree), builds only NimbleLauncher and EngineCore, and starts it.
rem Outside a developer prompt it runs vcvarsall from the newest Visual Studio
rem with C++ (vswhere). Needs cmake and Ninja on PATH (vcvarsall puts Visual
rem Studio's bundled ones there), or %CMAKE%, and Qt 6.8+ where CMake finds it
rem (Qt6_DIR or CMAKE_PREFIX_PATH).
setlocal
set "HERE=%~dp0"
set "BUILD=%HERE%..\Build\launcher"

rem No parenthesised blocks here: "Program Files (x86)" would close them.
if defined INCLUDE goto have_compiler
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto have_compiler
set "VSROOT="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT goto have_compiler
echo NimbleLauncher: setting up MSVC (vcvarsall x64)
rem vcvarsall runs vswhere itself, by name.
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
:have_compiler

if not defined CMAKE set "CMAKE=cmake"
where "%CMAKE%" >nul 2>nul || if not exist "%CMAKE%" (
    echo NimbleLauncher: cmake not found; install it or set CMAKE to its path 1>&2
    exit /b 1
)

if exist "%BUILD%\build.ninja" goto configured
echo NimbleLauncher: configuring Engine\Build\launcher -- the first time prebuilds the third-party libraries too
"%CMAKE%" -S "%HERE%.." -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNIMBLE_TARGET=Editor ^
    -DNIMBLE_BUILD_SANDBOX=OFF -DNIMBLE_BUILD_TESTS=OFF "-DNIMBLE_BINARIES_DIR=%BUILD%\Binaries" || exit /b 1
:configured
"%CMAKE%" --build "%BUILD%" --target NimbleLauncher || exit /b 1

set "EXE=%BUILD%\Binaries\Windows_RelWithDebInfo\NimbleLauncher.exe"
if exist "%EXE%" goto run
echo NimbleLauncher: built, but NimbleLauncher.exe is not in Engine\Build\launcher\Binaries\Windows_RelWithDebInfo 1>&2
exit /b 1
:run
start "" "%EXE%" %*
exit /b 0
