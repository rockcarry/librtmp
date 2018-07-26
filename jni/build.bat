@echo off
@echo building touchtouch library using ndk...
@echo.

set PATH=%PATH%;%NDK_HOME%
call ndk-build

@echo.
@echo build touchtouch library done !
@echo.

pause

