@echo off
REM push_v28.bat - Commit v2.8 and push to origin/master
REM All ASCII, no BOM, no Chinese in script body to avoid cmd encoding garble
REM Double-click to run. Pauses after every step.

setlocal
cd /d "%~dp0"
set "PATH=C:\Program Files\Git\cmd;%PATH%"

echo ============================================================
echo  push_v28.bat - v2.8 commit and push to origin/master
echo ============================================================
echo.
echo [self-check] git version:
git --version
if errorlevel 1 (
    echo [X] git NOT FOUND in C:\Program Files\Git\cmd
    pause
    exit /b 1
)
echo.
echo [self-check] current branch:
git branch --show-current
echo.
pause

echo ============================================================
echo  [1/4] Stage ALL changes, then unstage .claude/ and .bak
echo ============================================================
git add -A
git reset HEAD .claude/ >nul 2>&1
git reset HEAD "CMakeLists.txt.user.bak_20260903" >nul 2>&1
echo.
echo --- staged file count ---
git diff --cached --name-only | find /c /v ""
echo.
echo --- safety check: no .claude or .bak ---
git diff --cached --name-only | findstr /R "\.claude \.bak" >nul 2>&1
if not errorlevel 1 (
    echo [X] WARNING: .claude or .bak still staged! Aborting.
    git diff --cached --name-only | findstr /R "\.claude \.bak"
    pause
    exit /b 1
)
echo OK: no .claude / .bak staged
echo.
pause

echo ============================================================
echo  [2/4] Commit
echo ============================================================
git commit -m "v2.8 - SDK split + BatchModule multi-shard module + version unification + build tool separation" -m "- SDK split: sdk/ModuleBase.h + Export.h + sdk.h, wiring inversion (ForEachFunc host-side wiring)" -m "- SDK API version check: EXPORT_MODULE exports GetSdkApiVersion, LoadDLLModule validates on load" -m "- BatchModule: first multi-shard pauseable/resumable real module, cross-process restart recovery verified" -m "- TaskFilePersistence: file persistence backend, supports cross-process restart recovery" -m "- all built-in modules migrated to ModuleBase; version unified to v2.8" -m "- 507 tests / 72 suites" -m "- add rebuild_release.bat; VS2022/Qt build-dir separation fixes ninja dep-tracking bug" -m "Co-Authored-By: Claude Code <noreply@anthropic.com>"
echo.
echo commit exit code: %errorlevel% (0 = ok; non-zero ok if nothing to commit)
echo.
pause

echo ============================================================
echo  [3/4] Push to origin/master
echo ============================================================
git push origin master
echo.
echo push exit code: %errorlevel% (0 = success)
echo.
pause

echo ============================================================
echo  [4/4] Last 3 commits
echo ============================================================
git log --oneline -3
echo.
echo Done. If push exit code above is 0, push succeeded.
echo If push failed, run:  git pull --rebase origin master  then  git push origin master
echo.
pause
endlocal