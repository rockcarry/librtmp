#!/bin/sh

export DESTDIR=$PWD/_install
export prefix=
export SHARED=no
export CRYPTO=

case $1 in
mingw32)
    export XLDFLAGS="-lwinmm -lws2_32"
    make -j8 && make install
    ;;
linux)
    make -j8 && make install
    ;;
clean)
    make clean
    ;;
distclean)
    make clean
    rm -rf $PWD/_install
    ;;
esac
