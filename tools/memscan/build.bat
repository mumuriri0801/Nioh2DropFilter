@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /std:c++20 /O2 /W4 /EHsc /utf-8 /MT /Fo:bin\ /Fe:bin\memscan.exe memscan.cpp
