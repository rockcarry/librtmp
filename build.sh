#!/bin/sh

set -e

export DESTDIR=$PWD/_install
export prefix=
export SHARED=no
export CRYPTO=

case $1 in
mingw)
    export XCFLAGS="-D_WIN32"
    make SYS=mingw install -j8
    cd rtmppush
    gcc -o rtmppush.dll -shared -flto -I$DESTDIR/include rtmppush.c -L$DESTDIR/lib -lrtmp -lws2_32 -lwinmm
    strip --strip-unneeded rtmppush.dll
    dlltool -l rtmppush.lib -d rtmppush.def
    cd -
    ;;
linux)
    make install -j8
    cd rtmppush
    cp $DESTDIR/lib/librtmp.a .
    ${CROSS_COMPILE}gcc -c -I$DESTDIR/include rtmppush.c
    ${CROSS_COMPILE}ar rcs librtmp.a rtmppush.o
    cd -
    ;;
clean|distclean)
    make clean
    rm -rf $PWD/rtmppush/*.o $PWD/rtmppush/*.lib $PWD/rtmppush/*.dll $PWD/_install
    ;;
esac
