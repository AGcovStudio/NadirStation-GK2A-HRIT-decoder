@echo off
setlocal EnableExtensions
chcp 65001 >nul
rem ===========================================================================================================
rem  Build NadirStation GK-2A HRIT Decoder, collect the runtime files, make the installer.
rem  Run from anywhere:   scripts\build_release.bat
rem  Needs: Qt 6 (MinGW 64-bit) + CMake + MinGW from the Qt installer, and Inno Setup 6 for the installer
rem         (download: https://jrsoftware.org/isdl.php ).
rem  Edit the three paths below if your Qt is somewhere else.
rem ===========================================================================================================
set "QT_DIR=C:\Qt\6.11.2\mingw_64"
set "MINGW_DIR=C:\Qt\Tools\mingw1310_64"
set "CMAKE_DIR=C:\Qt\Tools\CMake_64"
set "PATH=%CMAKE_DIR%\bin;%QT_DIR%\bin;%MINGW_DIR%\bin;%PATH%"

set "ROOT=%~dp0.."
pushd "%ROOT%"
set "ROOT=%CD%"
set "APPEXE=NadirStation_GK2A-HRIT_decoder.exe"
set "VERSION=1.0.4"

echo [1/4] Configure
cmake -S "%ROOT%" -B "%ROOT%\build" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" || goto :fail
echo [2/4] Build
cmake --build "%ROOT%\build" --parallel 4 || goto :fail

echo [3/4] Collect runtime files into dist\
if exist "%ROOT%\dist" rmdir /s /q "%ROOT%\dist"
mkdir "%ROOT%\dist" || goto :fail
copy /y "%ROOT%\build\%APPEXE%" "%ROOT%\dist\" >nul || goto :fail
windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%ROOT%\dist\%APPEXE%" || goto :fail
mkdir "%ROOT%\dist\resources\maps" || goto :fail
copy /y "%ROOT%\resources\maps\landmask.jpg" "%ROOT%\dist\resources\maps\" >nul || goto :fail
copy /y "%ROOT%\resources\maps\ne_10m_admin_0_countries_chn.geojson" "%ROOT%\dist\resources\maps\" >nul || goto :fail
copy /y "%ROOT%\installer\EULA.txt" "%ROOT%\dist\" >nul || goto :fail
copy /y "%ROOT%\installer\THIRD_PARTY_NOTICES.txt" "%ROOT%\dist\" >nul || goto :fail
copy /y "%ROOT%\installer\LGPL_COMPLIANCE.txt" "%ROOT%\dist\" >nul || goto :fail
mkdir "%ROOT%\dist\LICENSES" || goto :fail
copy /y "%ROOT%\installer\LICENSES\*.*" "%ROOT%\dist\LICENSES\" >nul || goto :fail
rem DirectX shader compiler DLLs are not needed by this widgets program; do not ship them.
if exist "%ROOT%\dist\dxcompiler.dll" del /q "%ROOT%\dist\dxcompiler.dll"
if exist "%ROOT%\dist\dxil.dll" del /q "%ROOT%\dist\dxil.dll"
rem The decryption key is NOT shipped: every user supplies their own EncryptionKeyMessage.bin.

echo [4/4] Installer
set "ISCC="
for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\find_iscc.ps1"`) do set "ISCC=%%I"
if "%ISCC%"=="" (
  echo.
  echo Inno Setup 6 was not found, so no installer was made. The program itself is ready in dist\ .
  echo Install Inno Setup 6 from https://jrsoftware.org/isdl.php and run this script again.
  goto :end
)
"%ISCC%" "%ROOT%\installer\NadirStation_GK2A-HRIT_decoder.iss" || goto :fail

echo.
echo Done:  %ROOT%\installer\output\NadirStation_GK2A-HRIT_decoder_Setup_%VERSION%.exe
goto :end

:fail
echo.
echo *** Build failed (step above). ***
popd
exit /b 1

:end
popd
endlocal
