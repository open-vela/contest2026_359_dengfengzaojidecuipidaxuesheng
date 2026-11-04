@echo off
cd /d "%~dp0"
where pythonw >nul 2>nul
if not errorlevel 1 (
    start "" pythonw "%~dp0client\dht11_viewer.py" %*
) else (
    python "%~dp0client\dht11_viewer.py" %*
    if errorlevel 1 pause
)
