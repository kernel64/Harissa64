@echo off
rem Builds execmem.xex (M0.3 executable-memory proof of concept) with the
rem Xbox 360 SDK command-line tools, next to this script.
setlocal
if "%XEDK%"=="" set "XEDK=D:\App\Microsoft Xbox 360 SDK"
set "PATH=%XEDK%\bin\win32;%PATH%"
set "INCLUDE=%XEDK%\include\xbox;%XEDK%\include\xbox\sys"
set "LIB=%XEDK%\lib\xbox"
cd /d "%~dp0"
cl /nologo /c /TP /EHa /O2 /MT /GS- /D_XBOX /DNDEBUG /Zi /Fdexecmem.pdb execmem.cpp || exit /b 1
rem /SECTION:.jitc,ERW: method 9 needs a writable and executable image section.
link /nologo /OUT:execmem.exe /DEBUG /PDB:execmem_link.pdb /SECTION:.jitc,ERW execmem.obj xapilib.lib d3d9.lib xboxkrnl.lib xgraphics.lib || exit /b 1
imagexex /IN:execmem.exe /OUT:execmem.xex /CONFIG:xex.xml >nul || exit /b 1
echo BUILD OK: %~dp0execmem.xex
