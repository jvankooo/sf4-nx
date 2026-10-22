@echo off
rem Builds the host code generator (rexglue.exe) with clang inside the VS 2022 environment.
rem   tools\build_host.bat            ->  sdk\out\host\rexglue.exe
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"&& call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set ROOT=%~dp0..
if not exist "%ROOT%\sdk\out\host\CMakeCache.txt" (
  cmake -S "%ROOT%\sdk" -B "%ROOT%\sdk\out\host" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 || exit /b 1
)
cmake --build "%ROOT%\sdk\out\host" --target rexglue -j 12
