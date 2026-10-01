@echo off
rem Replaces the WHOLE history of this repository with one fresh commit that carries no personal data,
rem and force-pushes it to GitHub.
rem Why: the earlier commits contained a real name and email in github.cmd and in git's author field.
rem Afterwards the old commits are unreachable; GitHub drops them from its caches over time, and support can
rem purge them at once: https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository
rem Also worth switching on at https://github.com/settings/emails: "Keep my email addresses private" and
rem "Block command line pushes that expose my email".
setlocal EnableExtensions
cd /d "%~dp0"
where git >nul 2>&1 || (echo git not found & pause & exit /b 1)
where gh >nul 2>&1 || (echo GitHub CLI ^(gh^) not found - https://cli.github.com & pause & exit /b 1)

set "LOGIN="
for /f "usebackq delims=" %%l in (`gh api user --jq .login 2^>nul`) do set "LOGIN=%%l"
if not defined LOGIN (echo gh is not signed in - run: gh auth login & pause & exit /b 1)

set "VER="
for /f tokens^=2^ delims^=^" %%v in ('findstr /c:"kVersion = " "Milanote++\Util.h"') do set "VER=%%v"
if not defined VER set "VER=unknown"

rem refuse to run while the old hard-coded identity is still in github.cmd
if exist github.cmd findstr /i /c:"@gmail.com" github.cmd >nul 2>&1 && (
  echo github.cmd still contains a hard-coded email address - get the clean version first.
  pause
  exit /b 1
)

echo This replaces ALL history of the repository with ONE clean commit, authored as
echo   %LOGIN% ^<%LOGIN%@users.noreply.github.com^>
echo and force-pushes it to origin. The old commits ^(with the personal data^) become unreachable.
set /p OK=Type YES to continue:
if /i not "%OK%"=="YES" exit /b 1

git config user.name "%LOGIN%"
git config user.email "%LOGIN%@users.noreply.github.com"

git checkout --orphan clean-history >nul 2>&1 || (echo could not create the clean branch & pause & exit /b 1)
git add -A
git commit -m "Milanote++ %VER%" || (echo commit failed & pause & exit /b 1)
git branch -D main >nul 2>&1
git branch -m main
git push --force -u origin main || (echo push failed & pause & exit /b 1)

rem tags would keep the old commits alive on GitHub
for /f "delims=" %%t in ('git tag -l') do (
  git tag -d "%%t" >nul
  git push origin ":refs/tags/%%t" >nul 2>&1
)
git reflog expire --expire=now --all
git gc --prune=now --quiet

echo.
echo Done: one commit, no personal data. Check https://github.com/%LOGIN%/milanote-plus-plus/commits/main
echo If you want GitHub to purge the old commits immediately, open a support request with the link above.
pause
