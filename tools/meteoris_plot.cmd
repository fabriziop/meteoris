@echo off
setlocal
where py >nul 2>nul
if not errorlevel 1 (
    py -3 "%~dp0meteoris_plot.py" %*
    exit /b
)
python "%~dp0meteoris_plot.py" %*
exit /b
