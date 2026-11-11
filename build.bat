@echo off
rem Local MSVC build with Meson: build\AutoLevels.dll for AviSynth+ and VapourSynth (x64, static runtime).
rem Needs meson and ninja on PATH (e.g. pip install meson ninja).
setlocal
cd /d "%~dp0"
if not exist build\build.ninja (
  meson setup build --vsenv -Db_vscrt=mt || exit /b 1
)
meson compile -C build || exit /b 1
