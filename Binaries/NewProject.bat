@echo off
rem A new game from Engine\Templates\Game, without the editor (NewProject.cmake).
rem
rem     Engine\Binaries\NewProject.bat <Name> [<folder>]
rem     Engine\Binaries\NewProject.bat --example <Example> [--build [--jobs <N>]] [<Name> [<folder>]]
rem
rem <folder> defaults to Games\ beside Engine\. --example unpacks an example
rem project from Engine\Examples instead (its own name unless <Name> is given);
rem --build then configures, builds and cooks it, --jobs capping the build's jobs. Needs cmake on PATH (or %CMAKE%).
setlocal

set "EXAMPLE="
set "BUILD=OFF"
set "JOBS="
:options
if /i "%~1"=="--example" (
    if "%~2"=="" (
        echo %~nx0: --example needs a name 1>&2
        exit /b 2
    )
    set "EXAMPLE=%~2"
    shift
    shift
    goto options
)
if /i "%~1"=="--jobs" (
    set "JOBS=%~2"
    shift
    shift
    goto options
)
if /i "%~1"=="--build" (
    set "BUILD=ON"
    shift
    goto options
)

if not defined EXAMPLE if "%~1"=="" (
    echo Usage: %~nx0 ^<Name^> [^<folder^>] 1>&2
    echo        %~nx0 --example ^<Example^> [--build [--jobs ^<N^>]] [^<Name^> [^<folder^>]] 1>&2
    exit /b 2
)

if not defined CMAKE set "CMAKE=cmake"
where "%CMAKE%" >nul 2>nul || if not exist "%CMAKE%" (
    echo NewProject: cmake not found; install it or set CMAKE to its path 1>&2
    exit /b 1
)

"%CMAKE%" "-DNAME=%~1" "-DLOCATION=%~2" "-DEXAMPLE=%EXAMPLE%" "-DBUILD=%BUILD%" "-DJOBS=%JOBS%" -P "%~dp0NewProject.cmake"
exit /b %ERRORLEVEL%
