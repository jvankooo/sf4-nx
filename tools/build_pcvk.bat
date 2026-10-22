@echo off
rem Builds the PC (Windows) version with the SDK's Vulkan backend, for the native renderer: app\out\pcvk\sf4.exe
rem (tools\build_pc.bat is the Direct3D 12 build of the Xenos emulation). Needs out\native-shaders
rem (tools\build_native_shaders.sh) and, to draw, sf4_shaders.mesp + .idx next to sf4.exe (tools\build_shader_library.py).
rem   tools\build_pcvk.bat
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"&& call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set ROOT=%~dp0..
if not exist "%ROOT%\app\out\pcvk\CMakeCache.txt" (
  cmake -S "%ROOT%\app" -B "%ROOT%\app\out\pcvk" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 ^
    -DREXGLUE_USE_VULKAN=ON -DSF4_D3D_TRACE=OFF ^
    "-DREXSDK_DIR=%ROOT%\sdk" || exit /b 1
)
cmake --build "%ROOT%\app\out\pcvk" -j 10
