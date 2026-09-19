@echo off
setlocal
where py >nul 2>nul
if not errorlevel 1 (
    py -3 "%~dp0meteoris_config.py" %*
    exit /b
)
python "%~dp0meteoris_config.py" %*
exit /b
