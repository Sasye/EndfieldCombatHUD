@echo off
setlocal
cd /d "%~dp0"
if not defined APPLEPIE_SDK_DIR set "APPLEPIE_SDK_DIR=%~dp0deps\applepie_sdk"
if not exist "%APPLEPIE_SDK_DIR%\applepie_client.h" (
    echo [ERROR] Applepie SDK not found at "%APPLEPIE_SDK_DIR%".
    echo [ERROR] Restore deps\applepie_sdk or set APPLEPIE_SDK_DIR to a compatible SDK directory.
    exit /b 1
)
where cl >nul 2>nul
if errorlevel 1 (
    if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
        echo [ERROR] Use an x64 MSVC Developer Command Prompt.
        exit /b 1
    )
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
)
where cl >nul 2>nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
rc /nologo /fo bin\version.res src\version.rc
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /O2 /MD /LD /EHsc /std:c++17 /W4 /wd4505 /wd4996 ^
    /I"%APPLEPIE_SDK_DIR%" src\combat_hud.cpp bin\version.res user32.lib gdi32.lib ^
    /Fo"bin\\" /Fe:bin\EndfieldCombatHUD.dll ^
    /link /DLL /INCREMENTAL:NO /OPT:NOICF /MAP:bin\EndfieldCombatHUD.map
if errorlevel 1 exit /b 1
echo [OK] Built %CD%\bin\EndfieldCombatHUD.dll
exit /b 0
