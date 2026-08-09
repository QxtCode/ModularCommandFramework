@echo off
call "E:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul
cd /d "E:\CProjects\cpp\test_shell"
rmdir /s /q "out\build\x64-Debug" 2>nul
mkdir "out\build\x64-Debug"
"E:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B "out\build\x64-Debug" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_STANDARD=20
"E:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "out\build\x64-Debug" --target test_runner -j 8
