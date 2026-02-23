@echo off
setlocal

:: 1. CHECK FOR COMPILER (cl)
where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [INFO] Loading Developer Command Prompt...
    :: Using 'call' to import variables into this session
	call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -no_logo
)
echo.


:: 2. BUILD RESOURCE FILE
echo [INFO] Generating GyroScroll.res from GyroScroll.rc...
echo.
rc.exe GyroScroll.rc

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    echo  BUILD FAILED: Check code errors.
    echo !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    pause
    exit /b 1
)



:: 3. COMPILE (Fastest Method)
echo [INFO] Compiling GyroScroll with /O2 optimization...
echo.
cl /O2 /EHsc GyroScroll.cpp GyroScroll.res

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    echo  COMPILE FAILED: Check code errors.
    echo !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    pause
    exit /b 1
)

echo.
echo [SUCCESS]
echo.
pause
