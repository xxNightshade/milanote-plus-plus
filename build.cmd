@echo off
rem Builds Milanote++ (Release x64) from the command line and writes build.log next to this script.
rem Double-click it, or run it from any terminal. Requires Visual Studio 2022/2026 with the C++ workload.
setlocal EnableExtensions
set "ROOT=%~dp0"
cd /d "%ROOT%"
set "LOG=%ROOT%build.log"
echo Milanote++ build started %date% %time% > "%LOG%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
)
if not defined MSBUILD (
  for %%p in ("%ProgramFiles%\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe") do (
    if exist "%%~p" set "MSBUILD=%%~p"
  )
)
if not defined MSBUILD (
  echo MSBuild.exe not found - install Visual Studio with the "Desktop development with C++" workload. >> "%LOG%"
  type "%LOG%"
  pause
  exit /b 1
)
echo Using %MSBUILD% >> "%LOG%"

rem a running Milanote++ (setup window or MCP server) would keep the exe locked
taskkill /im Milanote++.exe /f >> "%LOG%" 2>&1

rem SolutionDir is read by MSBuild from the environment (a -p: value ending in a backslash would break quoting)
set "SolutionDir=%ROOT%"

echo === restore === >> "%LOG%"
"%MSBUILD%" "Milanote++.slnx" -t:Restore -p:RestorePackagesConfig=true -p:Configuration=Release -p:Platform=x64 -nologo -v:m >> "%LOG%" 2>&1
echo restore exit code %errorlevel% >> "%LOG%"

echo === build === >> "%LOG%"
"%MSBUILD%" "Milanote++.slnx" -t:Build -p:Configuration=Release -p:Platform=x64 -nologo -v:m >> "%LOG%" 2>&1
set "RC=%errorlevel%"
echo build exit code %RC% >> "%LOG%"
if "%RC%"=="0" (
  echo. >> "%LOG%"
  echo Output: %ROOT%x64\Release\Milanote++.exe >> "%LOG%"
  rem keep the copy on the Desktop (the one registered with Claude) up to date
  if exist "%USERPROFILE%\Desktop\Milanote++.exe" (
    copy /y "%ROOT%x64\Release\Milanote++.exe" "%USERPROFILE%\Desktop\Milanote++.exe" > nul && echo Updated %USERPROFILE%\Desktop\Milanote++.exe >> "%LOG%"
  )
)
echo Milanote++ build finished %date% %time% >> "%LOG%"
type "%LOG%"
if not "%RC%"=="0" pause
exit /b %RC%
