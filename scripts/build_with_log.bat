@echo off
rem Runs build_release.bat and saves everything it prints to build_log.txt in the project folder.
call "%~dp0build_release.bat" > "%~dp0..\build_log.txt" 2>&1
set "RC=%ERRORLEVEL%"
type "%~dp0..\build_log.txt"
echo.
if "%RC%"=="0" (echo Build finished. See build_log.txt.) else (echo Build FAILED. See build_log.txt.)
pause
exit /b %RC%
