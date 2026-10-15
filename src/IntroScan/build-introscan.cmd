@echo off
rem Builds mpc-introscan.exe next to mpc-hc64.exe. Needs the player built first (LAV Filters FFmpeg
rem import libraries and generated headers come from its build tree).
setlocal
set NoDefaultCurrentDirectoryInExePath=
set ROOT=%~dp0..\..
set LAV=%ROOT%\src\thirdparty\LAVFilters\src
set OUT=%ROOT%\bin\mpc-hc_x64
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo || exit /b 1
if not exist "%OUT%\obj-introscan" mkdir "%OUT%\obj-introscan"
cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /utf-8 /DUNICODE /D_UNICODE ^
  /I"%LAV%\ffmpeg" /I"%LAV%\bin_x64\thirdparty\ffmpeg" ^
  /Fo"%OUT%\obj-introscan\\" /Fe"%OUT%\mpc-introscan.exe" "%~dp0IntroScan.cpp" ^
  /link /SUBSYSTEM:CONSOLE "%LAV%\bin_x64\lib\avformat-lav.lib" "%LAV%\bin_x64\lib\avcodec-lav.lib" ^
  "%LAV%\bin_x64\lib\avutil-lav.lib" "%LAV%\bin_x64\lib\swresample-lav.lib" user32.lib shell32.lib shlwapi.lib ole32.lib delayimp.lib ^
  /DELAYLOAD:avformat-lav-63.dll /DELAYLOAD:avcodec-lav-63.dll /DELAYLOAD:avutil-lav-61.dll /DELAYLOAD:swresample-lav-7.dll || exit /b 1
echo Built %OUT%\mpc-introscan.exe
