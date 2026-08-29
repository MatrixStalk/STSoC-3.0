@echo off
setlocal
set "TOOL_DIR=%~dp0"
set "CODEX_PYW=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\pythonw.exe"

if exist "%CODEX_PYW%" (
    start "" "%CODEX_PYW%" "%TOOL_DIR%xray_bump_tool.pyw" %*
    exit /b 0
)

where pythonw.exe >nul 2>nul
if not errorlevel 1 (
    start "" pythonw.exe "%TOOL_DIR%xray_bump_tool.pyw" %*
    exit /b 0
)

echo Python with Pillow and NumPy was not found.
echo Run the tool from Codex or install Python 3, Pillow and NumPy.
pause
exit /b 1
