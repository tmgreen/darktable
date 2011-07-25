#!/bin/sh

# Change this to reflect your setup
# Also edit Toolchain-mingw32.cmake
MINGW="/usr/i586-mingw32msvc"
RUNTIME_PREFIX="Z:/opt/darktable-win"


export CFLAGS="-mms-bitfields"
export LDFLAGS="-Wl,--enable-runtime-pseudo-reloc -Wl,-subsystem,windows"
export PATH=${MINGW}/bin:$PATH
export CPATH=${MINGW}/include:${MINGW}/include/OpenEXR/
export LD_LIBRARY_PATH=${MINGW}/lib
export LD_RUN_PATH=${MINGW}/lib
export PKG_CONFIG_LIBDIR=${MINGW}/lib/pkgconfig

DT_SRC_DIR=`dirname "$0"`
DT_SRC_DIR=`cd "$DT_SRC_DIR"; pwd`

cd $DT_SRC_DIR;

INSTALL_PREFIX=$1
if [ "$INSTALL_PREFIX" =  "" ]; then
	INSTALL_PREFIX=/opt/darktable-win/
fi

BUILD_TYPE=$2
if [ "$BUILD_TYPE" =  "" ]; then
        BUILD_TYPE=Release
fi

echo "Installing to $INSTALL_PREFIX for $BUILD_TYPE"
echo "WARNING: This is a highly experimental try to cross compile darktable for Windows!"
echo "         It is not guaranteed to compile or do anything useful when executed!"
echo ""

if [ ! -d build/ ]; then
	mkdir build/
fi

cd build/

MAKE_TASKS=1
if [ -r /proc/cpuinfo ]; then
	MAKE_TASKS=$(grep -c "^processor" /proc/cpuinfo)
elif [ -x /sbin/sysctl ]; then
	TMP_CORES=$(/sbin/sysctl -n hw.ncpu 2>/dev/null)
	if [ "$?" = "0" ]; then
		MAKE_TASKS=$TMP_CORES
	fi
fi

if [ "$(($MAKE_TASKS < 1))" -eq 1 ]; then
	MAKE_TASKS=1
fi

cmake -DCMAKE_TOOLCHAIN_FILE=../Toolchain-mingw32.cmake -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} -DRUNTIME_INSTALL_PREFIX=${RUNTIME_PREFIX} -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DINSTALL_IOP_EXPERIMENTAL=Off -DINSTALL_IOP_LEGACY=Off -DDONT_USE_RAWSPEED=1 .. && make -j $MAKE_TASKS

if [ $? = 0 ]; then
	echo "Darktable finished building, to actually install darktable you need to type:"
	echo "# cd build; sudo make install"
fi
