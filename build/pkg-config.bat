@echo off
if "%~1"=="--version" (
  echo 0.29.2
  exit /b 0
)
echo %* | findstr /i "freetype2" >nul
if errorlevel 1 exit /b 1
if "%~1"=="--exists" exit /b 0
if "%~1"=="--cflags" (
  echo -I%FT_PREFIX%/include/freetype2
  exit /b 0
)
if "%~1"=="--libs" (
  echo -L%FT_PREFIX%/lib -lfreetype
  exit /b 0
)
if "%~1"=="--modversion" (
  echo 26.1.20
  exit /b 0
)
if "%~1"=="--variable=prefix" (
  echo %FT_PREFIX%
  exit /b 0
)
exit /b 0
