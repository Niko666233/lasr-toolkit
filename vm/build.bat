@echo off
rem Build tufa_vm.exe — MUST be x86 (32-bit pointers stored in u32 slots)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 (echo vcvarsall failed & exit /b 1)
cl /nologo /O2 /W3 /Fe:tufa_vm.exe tufa_vm.c
exit /b %errorlevel%
