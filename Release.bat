@echo off
setlocal

:: Check for arguments
if "%~1"=="" (
    echo Usage: release.bat [version] "comment"
    exit /b 1
)
set VERSION=%~1
set MSG=%~2

:: If no comment is provided, use a default
if "%MSG%"=="" set MSG=Release %VERSION%

echo.
echo ========================================
echo  Releasing GyroScroll version %VERSION%
echo ========================================

git add .
git commit -m "%MSG%"

:: -f allows you to overwrite the tag if it already exists
git tag -f %VERSION% -m "%MSG%"

echo.
echo Pushing to GitHub...
:: Pushing the branch and then forcing the tag update on the server
git push origin main --force && git push origin %VERSION% --force

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    echo !!      ERROR: Git Push failed        !!
    echo !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    pause
    exit /b 1
)

echo.
echo ===========================================
echo  SUCCESS! Tag has been updated to %VERSION%
echo ===========================================
pause