@echo off
setlocal enabledelayedexpansion

echo Building xaml2winuic C++23 Modules (VS 2026 / MSVC v18)
echo

where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    for %%V in ("18" "17" "2026" "2022") do (
        for %%E in ("Community" "Professional" "Enterprise" "BuildTools") do (
            if exist "%ProgramFiles%\Microsoft Visual Studio\%%~V\%%~E\VC\Auxiliary\Build\vcvarsall.bat" (
                call "%ProgramFiles%\Microsoft Visual Studio\%%~V\%%~E\VC\Auxiliary\Build\vcvarsall.bat" x64
                goto :vcvars_done
            )
            if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%~V\%%~E\VC\Auxiliary\Build\vcvarsall.bat" (
                call "%ProgramFiles(x86)%\Microsoft Visual Studio\%%~V\%%~E\VC\Auxiliary\Build\vcvarsall.bat" x64
                goto :vcvars_done
            )
        )
    )
)
:vcvars_done

set "PRJ_ROOT=%~dp0"
if "%PRJ_ROOT:~-1%"=="\" set "PRJ_ROOT=%PRJ_ROOT:~0,-1%"

pushd "%PRJ_ROOT%"

echo [Step 1] Incremental module build check...

echo [Step 2] Compiling C++23 Modules with MSVC /std:c++latest...

set FLAGS=/nologo /O2 /Gy /Gw /GL /DNDEBUG /utf-8 /std:c++latest /EHsc /c /ifcSearchDir .

cl %FLAGS% "%PRJ_ROOT%\registry.ixx" "%PRJ_ROOT%\xaml.ixx" "%PRJ_ROOT%\lang.ixx" "%PRJ_ROOT%\extensions.ixx" "%PRJ_ROOT%\convert.ixx" "%PRJ_ROOT%\cli.ixx" || goto :error

echo [Step 3] Linking CLI executable in Release mode...

if exist main.obj del main.obj

cl /nologo /O2 /Gy /Gw /GL /DNDEBUG /utf-8 /std:c++latest /EHsc /ifcSearchDir . /Fe"xaml2winuic.exe" "%PRJ_ROOT%\main.cpp" registry.obj xaml.obj lang.obj extensions.obj convert.obj cli.obj /link /LTCG /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTINPUT:"%PRJ_ROOT%\xaml2winuic.manifest" ole32.lib

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Linking failed!
    popd
    exit /b %ERRORLEVEL%
)

echo [Step 4] Intermediate object and IFC files retained for fast incremental builds.
if exist "%PRJ_ROOT%\build" rmdir /s /q "%PRJ_ROOT%\build"

echo [Step 5] Build complete!
echo   CLI Executable : %PRJ_ROOT%\xaml2winuic.exe
popd

echo Build Complete - Run: xaml2winuic.exe --help
exit /b 0

:error
echo [ERROR] Module compilation failed!
popd
exit /b 1
