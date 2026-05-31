@echo off
setlocal enabledelayedexpansion

REM Paths to compiler and SDK
set CLPATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe
set LIBVCPATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\lib\x64
set INCVCPATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\include
set SDKBASE=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0
set LIBUMPATH=%SDKBASE%\Lib\%SDKVER%\um\x64
set LIBUCRTPATH=%SDKBASE%\Lib\%SDKVER%\ucrt\x64
set INCUMPATH=%SDKBASE%\Include\%SDKVER%\um
set INCSHAREDPATH=%SDKBASE%\Include\%SDKVER%\shared
set INCUCRTPATH=%SDKBASE%\Include\%SDKVER%\ucrt

REM Create output directory
if not exist x64\Debug mkdir x64\Debug

REM Compile central
echo Compilando central...
cd central
"%CLPATH%" /W3 /TC /D_UNICODE /DUNICODE "/I%INCVCPATH%" "/I%INCUMPATH%" "/I%INCSHAREDPATH%" "/I%INCUCRTPATH%" central.c "/link" "/LIBPATH:%LIBVCPATH%" "/LIBPATH:%LIBUMPATH%" "/LIBPATH:%LIBUCRTPATH%" advapi32.lib kernel32.lib "/out:central.exe"
if errorlevel 1 (
    echo Erro ao compilar central!
    exit /b 1
)
if exist central.exe move central.exe ..\x64\Debug\central.exe >nul
cd ..

REM Compile placar
echo Compilando placar...
cd placar
"%CLPATH%" /W3 /TC /D_UNICODE /DUNICODE "/I%INCVCPATH%" "/I%INCUMPATH%" "/I%INCSHAREDPATH%" "/I%INCUCRTPATH%" placar.c "/link" "/LIBPATH:%LIBVCPATH%" "/LIBPATH:%LIBUMPATH%" "/LIBPATH:%LIBUCRTPATH%" advapi32.lib kernel32.lib "/out:placar.exe"
if errorlevel 1 (
    echo Erro ao compilar placar!
    exit /b 1
)
if exist placar.exe move placar.exe ..\x64\Debug\placar.exe >nul
cd ..

echo Compilacao concluida com sucesso!
echo Os executaveis ficam em: x64\Debug\central.exe e x64\Debug\placar.exe
