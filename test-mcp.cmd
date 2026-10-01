@echo off
rem Smoke-tests the built Milanote++.exe without Claude:
rem   1. registers it in Claude Desktop's config (idempotent)
rem   2. talks MCP to "Milanote++.exe --mcp" over stdin/stdout and records the answers in mcp-test.log
rem      test-mcp.cmd       -> test-mcp.jsonl: read-only session (tools/prompts/resources lists, the guide,
rem                            whoami, a template, the board tree, Home exported as an outline, a search)
rem      test-mcp.cmd gdd   -> test-gdd.jsonl: additionally builds the example Tic Tac Toe GDD
rem                            (docs\examples\tic-tac-toe-gdd.md) as a new board inside Home with ONE
rem                            milanote_build_board call
rem Requires a Milanote sign-in done earlier through the setup window.
setlocal EnableExtensions
cd /d "%~dp0"
set "REQ=test-mcp.jsonl"
if /i "%~1"=="gdd" set "REQ=test-gdd.jsonl"
set "EXE=%USERPROFILE%\Desktop\Milanote++.exe"
if not exist "%EXE%" set "EXE=%~dp0x64\Release\Milanote++.exe"
if not exist "%EXE%" (
  echo Build first: %EXE% not found
  pause
  exit /b 1
)
echo Milanote++ MCP smoke test %date% %time% > mcp-test.log
echo exe: %EXE% >> mcp-test.log

echo === --install === >> mcp-test.log
start "" /wait "%EXE%" --install
echo install exit code %errorlevel% >> mcp-test.log

echo === --mcp session === >> mcp-test.log
rem the ping keeps stdin open while the server answers (closing it ends the session)
echo requests: %REQ% >> mcp-test.log
(
  type "%REQ%"
  ping -n 76 127.0.0.1 > nul
) | "%EXE%" --mcp >> mcp-test.log 2> mcp-test.err
echo mcp exit code %errorlevel% >> mcp-test.log
echo done >> mcp-test.log
echo Finished - see mcp-test.log
