@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\Users\skyle\src\nrfilter\build
echo === dependents nrfilter.exe
dumpbin /dependents nrfilter.exe | findstr /i ".dll"
echo === imports nrfilter.exe (user32/kernel32 names)
dumpbin /imports nrfilter.exe | findstr /i /r "OpenProcess ReadProcessMemory WriteProcessMemory CreateRemoteThread SetWindowsHookEx VirtualAllocEx NtQuery DebugActiveProcess SetThreadContext GetAsyncKeyState SendInput keybd_event mouse_event SetWindowDisplayAffinity RegisterHotKey FindWindow EnumWindows DwmGetWindowAttribute"
echo === imports forwarder
dumpbin /imports nvngx.dll_nrfilter.dll | findstr /i /r "OpenProcess ReadProcessMemory SetWindowsHookEx CreateRemoteThread LoadLibrary"
