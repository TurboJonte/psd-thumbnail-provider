@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE ^
   decode_test.cpp ..\PsdThumbnailProvider\PsdDecoder.cpp ^
   /Fe:decode_test.exe /Fo:obj\ /link /SUBSYSTEM:CONSOLE gdi32.lib
