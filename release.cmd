@echo off
rem Publishes a Milanote++ release on GitHub - exactly what the built-in updater looks for:
rem   1. reads the version from Milanote++\Util.h (kVersion); bump it there first
rem   2. builds Release x64 (build.cmd)
rem   3. writes release\Milanote++.exe and release\Milanote++.exe.sha256
rem   4. commits, tags v<version>, pushes, and creates the GitHub release with both assets
rem Requires git and the GitHub CLI (run "gh auth login" once). Release notes come from
rem release-notes.md when that file exists, otherwise GitHub generates them from the commits.
setlocal EnableExtensions
cd /d "%~dp0"

set "VER="
for /f tokens^=2^ delims^=^" %%v in ('findstr /c:"kVersion = " "Milanote++\Util.h"') do set "VER=%%v"
if not defined VER (
  echo Could not read kVersion from Milanote++\Util.h
  pause
  exit /b 1
)
echo Releasing Milanote++ %VER%

where gh >nul 2>&1 || (echo GitHub CLI ^(gh^) not found - https://cli.github.com & pause & exit /b 1)
where git >nul 2>&1 || (echo git not found & pause & exit /b 1)
git tag -l "v%VER%" | findstr /x "v%VER%" >nul && (echo Tag v%VER% already exists - bump kVersion in Util.h first & pause & exit /b 1)

call build.cmd
if errorlevel 1 (echo Build failed & pause & exit /b 1)

if not exist release mkdir release
copy /y "x64\Release\Milanote++.exe" "release\Milanote++.exe" >nul || (echo Copy failed & pause & exit /b 1)

set "HASH="
for /f "skip=1 tokens=1" %%h in ('certutil -hashfile "release\Milanote++.exe" SHA256') do if not defined HASH set "HASH=%%h"
if not defined HASH (echo Could not hash the exe & pause & exit /b 1)
> "release\Milanote++.exe.sha256" echo %HASH%  Milanote++.exe
echo SHA-256 %HASH%

rem commit identity: never a real email by accident (it is public with every push)
set "EMAIL="
for /f "usebackq delims=" %%e in (`git config user.email 2^>nul`) do set "EMAIL=%%e"
if not defined EMAIL (
  for /f "usebackq delims=" %%l in (`gh api user --jq .login 2^>nul`) do set "LOGIN=%%l"
  if not defined LOGIN (echo git has no identity and gh is not signed in & pause & exit /b 1)
  git config user.name "%LOGIN%"
  git config user.email "%LOGIN%@users.noreply.github.com"
) else (
  echo %EMAIL% | findstr /i "users.noreply.github.com" >nul || echo NOTE: commits are made as %EMAIL% - this address is public on GitHub.
)

git add -A
git commit -m "Milanote++ %VER%" >nul 2>&1
git tag -a "v%VER%" -m "Milanote++ %VER%" || (echo Tagging failed & pause & exit /b 1)
git push origin HEAD --tags || (echo Push failed & pause & exit /b 1)

if exist release-notes.md (
  gh release create "v%VER%" "release\Milanote++.exe" "release\Milanote++.exe.sha256" --title "Milanote++ %VER%" --notes-file release-notes.md
) else (
  gh release create "v%VER%" "release\Milanote++.exe" "release\Milanote++.exe.sha256" --title "Milanote++ %VER%" --generate-notes
)
if errorlevel 1 (echo gh release create failed & pause & exit /b 1)
echo Published: https://github.com/xxNightshade/milanote-plus-plus/releases/tag/v%VER%
echo Every installed Milanote++ picks it up on its next start.
pause
