@echo off
rem Collects diagnostics next to this script (Claude Desktop config copies, Milanote++ log, process list).
cd /d "%~dp0"
del /q diag-*.txt diag-*.json diag-*.log 2>nul
copy /y "%APPDATA%\Claude\claude_desktop_config.json" "diag-config-appdata.json" > diag-copy.txt 2>&1
for /d %%p in ("%LOCALAPPDATA%\Packages\Claude_*") do (
  echo package: %%p >> diag-copy.txt
  dir "%%p\LocalCache\Roaming\Claude" >> diag-copy.txt 2>&1
  copy /y "%%p\LocalCache\Roaming\Claude\claude_desktop_config.json" "diag-config-package.json" >> diag-copy.txt 2>&1
)
copy /y "%LOCALAPPDATA%\Milanote++\milanote++.log" "diag-milanote++.log" >> diag-copy.txt 2>&1
tasklist /v /fi "imagename eq Milanote++.exe" > diag-tasklist.txt 2>&1
echo done > diag-done.txt
