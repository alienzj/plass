#!/bin/bash -e
REPO="$(greadlink -f $1)"
BUILD="$(greadlink -f $2)"

if [ ! -d "$REPO" ]; then
    echo "Plass repository missing"
    exit 1
fi

export CXX=${CXX:-g++-7}

mkdir -p "$BUILD/plass"
mkdir -p "$BUILD/lib"
cd "$BUILD/lib"
ln -s `$CXX --print-file-name=libgomp.a`
export CXXFLAGS="-L$BUILD/lib"

mkdir -p "$BUILD/build_sse41" && cd "$BUILD/build_sse41"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$BUILD/plass" -DHAVE_MPI=0 -DHAVE_SSE4_1=1 -DBUILD_SHARED_LIBS=OFF -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" -DCMAKE_FIND_LIBRARY_SUFFIXES=".a" "$REPO"
make -j 4
make install

cd "$BUILD"
tar -czvf plass-osx-static_sse41.tar.gz mmseqs

