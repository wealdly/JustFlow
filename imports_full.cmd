@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\Users\skyle\src\nrfilter\build
for %%f in (nrfilter.exe nvngx.dll_nrfilter.dll) do (
  echo ===== %%f
  dumpbin /imports %%f > %%f.imports.txt
)
