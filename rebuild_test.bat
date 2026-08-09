@echo off
call "E:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul
cd /d "E:\CProjects\cpp\test_shell"
"E:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "out\build\x64-Debug" --target test_runner -j 8
