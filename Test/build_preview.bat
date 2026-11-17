@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /DUNICODE /D_UNICODE ^
   preview_test.cpp ..\PsdThumbnailProvider\PsdPreviewHandler.cpp ..\PsdThumbnailProvider\PsdDecoder.cpp ^
   /Fe:preview_test.exe /Fo:obj3\ /link /SUBSYSTEM:CONSOLE
