@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE ^
   compare_test.cpp orig_GetThumbnail.cpp ..\PsdThumbnailProvider\PsdDecoder.cpp ^
   /Fe:compare_test.exe /Fo:obj2\ /link /SUBSYSTEM:CONSOLE gdi32.lib
