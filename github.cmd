@echo off
rem Commits this folder and pushes it to GitHub. Writes github.log next to this script.
rem   * with the GitHub CLI (gh) signed in: creates the repository (if there is no remote yet) and pushes
rem   * without gh: put the URL of an empty repository you created on github.com into github-remote.txt
rem     (next to this script) and run again; Git Credential Manager asks you to sign in on the first push.
rem Commit identity: this script never writes a name or an email into the repository. If git has no
rem identity configured, the GitHub "noreply" address of the signed-in gh account is used for this
rem repository only, so your real email never ends up in public commits.
setlocal EnableExtensions
cd /d "%~dp0"
set "LOG=%~dp0github.log"
set "REPO=milanote-plus-plus"
echo GitHub publish started %date% %time% > "%LOG%"
where git >> "%LOG%" 2>&1 || (echo git is not installed or not on PATH - install Git for Windows >> "%LOG%" & goto :fail)

if not exist ".git" git init -b main >> "%LOG%" 2>&1
call :identity || goto :fail

git add -A >> "%LOG%" 2>&1
git status --short >> "%LOG%" 2>&1
git commit -m "Milanote++ update" >> "%LOG%" 2>&1
echo local commit: >> "%LOG%"
git log --oneline -1 >> "%LOG%" 2>&1

git remote get-url origin > nul 2>&1
if not errorlevel 1 goto :push

if exist "github-remote.txt" (
  for /f "usebackq delims=" %%u in ("github-remote.txt") do set "REMOTE=%%u"
)
if defined REMOTE (
  echo using remote from github-remote.txt: %REMOTE% >> "%LOG%"
  git remote add origin "%REMOTE%" >> "%LOG%" 2>&1 || goto :fail
  goto :push
)

where gh > nul 2>&1
if errorlevel 1 (
  echo GitHub CLI ^(gh^) is not installed and github-remote.txt is missing. >> "%LOG%"
  echo Either install gh ^(winget install GitHub.cli, then: gh auth login^) or create an empty repo on github.com >> "%LOG%"
  echo and save its URL as the only line of github-remote.txt, then run this script again. >> "%LOG%"
  goto :fail
)
gh auth status >> "%LOG%" 2>&1 || (echo gh is not signed in - run: gh auth login >> "%LOG%" & goto :fail)
gh repo create "%REPO%" --public --source . --remote origin --description "Milanote for Claude: a Windows MCP server that lets Claude read and write your Milanote boards" >> "%LOG%" 2>&1 || goto :fail

:push
git push -u origin main >> "%LOG%" 2>&1 || goto :fail
git remote get-url origin >> "%LOG%" 2>&1
echo GitHub publish finished OK %date% %time% >> "%LOG%"
type "%LOG%"
exit /b 0

:identity
rem Leaves an existing identity alone, but warns when it is a real email address (it becomes public with every push).
set "EMAIL="
for /f "usebackq delims=" %%e in (`git config user.email 2^>nul`) do set "EMAIL=%%e"
if defined EMAIL (
  echo %EMAIL% | findstr /i "users.noreply.github.com" > nul || (
    echo NOTE: commits are made as %EMAIL%, which is visible to everyone on GitHub. >> "%LOG%"
    echo       For a private address run:  git config user.email "YOURNAME@users.noreply.github.com" >> "%LOG%"
  )
  exit /b 0
)
set "LOGIN="
where gh > nul 2>&1 && for /f "usebackq delims=" %%l in (`gh api user --jq .login 2^>nul`) do set "LOGIN=%%l"
if not defined LOGIN (
  echo git has no identity yet and gh is not signed in. Run once: >> "%LOG%"
  echo   git config --global user.name "YOURNAME" >> "%LOG%"
  echo   git config --global user.email "YOURNAME@users.noreply.github.com" >> "%LOG%"
  exit /b 1
)
git config user.name "%LOGIN%" >> "%LOG%" 2>&1
git config user.email "%LOGIN%@users.noreply.github.com" >> "%LOG%" 2>&1
echo commit identity for this repository: %LOGIN% ^<%LOGIN%@users.noreply.github.com^> >> "%LOG%"
exit /b 0

:fail
echo FAILED >> "%LOG%"
type "%LOG%"
pause
exit /b 1
