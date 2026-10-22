@echo off
rem Builds the PC (Windows) version of the port for testing: app\out\pc\sf4.exe
rem   tools\build_pc.bat
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"&& call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set ROOT=%~dp0..
if not exist "%ROOT%\app\out\pc\CMakeCache.txt" (
  cmake -S "%ROOT%\app" -B "%ROOT%\app\out\pc" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 ^
    "-DREXSDK_DIR=%ROOT%\sdk" || exit /b 1
)
cmake --build "%ROOT%\app\out\pc" -j 10
