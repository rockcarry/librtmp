@echo off
@echo building librtmp using ndk...
@echo.

set PATH=%PATH%;%NDK_HOME%
call ndk-build

@echo.
@echo build librtmp done !
@echo.

pause

