#!/bin/sh

set -e

# script for build binary release

TOPDIR=$PWD
REL_PLAT_LIST=("msc3xx-glibc" "msc3xx-uclibc" "axera-glibc")
TOOLCHAIN_LIST=("arm-linux-gnueabihf" "arm-buildroot-linux-uclibcgnueabihf" "arm-linux-gnueabihf")

function build_librtmp_by_platform()
{
    local PLATFORM=$1

    ./build.sh distclean
    ./build.sh linux

    mkdir -p $TOPDIR/tmp && cp -r $TOPDIR/rtmppush $TOPDIR/tmp/$PLATFORM
    rm -rf $TOPDIR/tmp/$PLATFORM/*.c
    rm -rf $TOPDIR/tmp/$PLATFORM/*.o
    rm -rf $TOPDIR/tmp/$PLATFORM/*.def
}

if [ "$1"x == "release"x ]; then
git fetch && git checkout .
CURRENT_BRANCH=`git rev-parse --abbrev-ref HEAD`
SOURCE_VERSION=`cat rtmppush/rtmppush.h | grep "// version:" | awk '{print $3}' | sed 's/[\"\r]//g'`
SOURCE_COMMITID=`git rev-parse HEAD` || true
REL_DATETIME=`date "+%Y-%m-%d %H:%M:%S"`
fi

rm -rf $TOPDIR/tmp
for i in "${!REL_PLAT_LIST[@]}"; do
echo "build for ${REL_PLAT_LIST[$i]} ..."
export CROSS_COMPILE=${TOOLCHAIN_LIST[$i]}-
export BUILD_HOST=${TOOLCHAIN_LIST[$i]}
rm -rf $TOPDIR/${REL_PLAT_LIST[$i]}
build_librtmp_by_platform ${REL_PLAT_LIST[$i]}
done

shopt -s extglob
if [ "$1"x == "release"x ]; then
git checkout origin/v1.x.x-bin-rel
rm -rf !(.|..|.git|.gitignore|tmp)
mv tmp/* . && rm -rf tmp
git add -A .

git commit -m "librtmp library binary release $SOURCE_VERSION." -m "build by source code commit id: $SOURCE_COMMITID" -m "build datetime: $REL_DATETIME"
git push origin HEAD:refs/for/v1.x.x-bin-rel
git checkout $CURRENT_BRANCH
else
find $TOPDIR/tmp -iname "*.la" | xargs rm -rf
mv $TOPDIR/tmp/* $TOPDIR
rm -rf $TOPDIR/tmp
fi

echo "finish."
