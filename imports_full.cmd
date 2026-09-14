@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0build
for %%f in (justflow.exe nvngx.dll_justflow.dll) do (
  echo ===== %%f
  dumpbin /imports %%f > %%f.imports.txt
)
