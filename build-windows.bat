@ECHO OFF
REM Configure and build this Mixxx fork with the VS 2022 Build Tools and the
REM prebuilt release dependencies in buildenv\. Run from the repository root.
SETLOCAL
CALL "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >NUL || EXIT /B 1
SET "MIXXX_VCPKG_ROOT=%~dp0buildenv\mixxx-deps-2.5-x64-windows-release-40c29ff"
SET "VCPKG_DEFAULT_TRIPLET=x64-windows-release"
SET "X_VCPKG_APPLOCAL_DEPS_INSTALL=ON"
IF NOT EXIST "%~dp0build\build.ninja" (
    cmake -G Ninja -S "%~dp0." -B "%~dp0build" ^
        -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
        -DVCPKG_TARGET_TRIPLET=x64-windows-release ^
        -DBATTERY=OFF -DBROADCAST=ON -DBULK=OFF -DHID=ON -DLILV=OFF ^
        -DOPTIMIZE=portable -DQTKEYCHAIN=OFF -DSTATIC_DEPS=OFF ^
        -DBUILD_TESTING=OFF -DBUILD_BENCH=OFF || EXIT /B 1
)
cmake --build "%~dp0build" --target mixxx || EXIT /B 1
ECHO BUILD SUCCEEDED
