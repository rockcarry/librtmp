#!/bin/sh

set -e

make prefix=$PWD/_install SYS=mingw
gcc -o rtmp.dll -shared -flto rtmppush.c *.o -lws2_32 -lwinmm && strip rtmp.dll
dlltool -l rtmp.lib -d rtmp.def

