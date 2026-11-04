@echo off
cd /d "%~dp0"
where pythonw >nul 2>nul
if not errorlevel 1 (
    start "" pythonw "%~dp0client\jbox_gui.py"
) else (
    python "%~dp0client\jbox_gui.py"
    if errorlevel 1 pause
)
