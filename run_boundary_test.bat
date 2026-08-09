@echo off
REM ============================================================
REM  Boundary test runner — pipes edge cases to test_shell.exe
REM  Run from VS Developer Command Prompt, or just double-click.
REM
REM  Memory leaks: check VS Output window (Debug) after run.
REM ============================================================
cd /d "%~dp0out\build\x64-Debug"
echo.
echo === Running boundary tests ===
echo.
type "..\..\..\test_boundary_inputs.txt" | test_shell.exe
echo.
echo === Done. Check VS Output window for memory leak dump ===
pause
