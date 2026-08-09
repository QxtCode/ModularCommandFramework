@echo off
call "E:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat" > NUL
cd /d "E:\C语言程序\c++项目\test_shell\test_shell"
"E:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build out/build/x64-Debug --target CalculatorModule
