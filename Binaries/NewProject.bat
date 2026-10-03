@echo off
rem A new game from Engine\Templates\Game, without the editor (NewProject.cmake).
rem
rem     Engine\Binaries\NewProject.bat <Name> [<folder>]
rem
rem <folder> defaults to Games\ beside Engine\. Needs cmake on PATH (or %CMAKE%).
setlocal

if "%~1"=="" (
    echo Usage: %~nx0 ^<Name^> [^<folder^>] 1>&2
    exit /b 2
)

if not defined CMAKE set "CMAKE=cmake"
where "%CMAKE%" >nul 2>nul || if not exist "%CMAKE%" (
    echo NewProject: cmake not found; install it or set CMAKE to its path 1>&2
    exit /b 1
)

"%CMAKE%" "-DNAME=%~1" "-DLOCATION=%~2" -P "%~dp0NewProject.cmake"
exit /b %ERRORLEVEL%
