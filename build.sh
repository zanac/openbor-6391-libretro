#!/bin/sh
# Portable build helper for the OpenBOR multi-engine libretro core and
# its standalone program.
#
#   ./build.sh                     native core build (Linux, macOS, MSYS2/MinGW)
#   ./build.sh win                 cross build of the core for Windows (x86_64-w64-mingw32-gcc)
#   ./build.sh standalone          native standalone program "openbor" (needs SDL2:
#                                  sdl2-config on the PATH, or SDL2_CONFIG=...)
#   ./build.sh win-standalone [SDL2-devel-mingw dir]
#                                  cross build of openbor.exe (SDL2 linked statically);
#                                  without a directory SDL2's MinGW development
#                                  package (version $SDL2_VERSION) is downloaded
#                                  into deps/sdl2-mingw
#   ./build.sh test <game.pak>     native build + headless smoke test
#
# Extra make variables can be passed through the environment, e.g.
#   DEBUG=1 ./build.sh   or   CC=clang ./build.sh
set -e
cd "$(dirname "$0")"
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
SDL2_VERSION=${SDL2_VERSION:-2.30.9}

case "$1" in
  win)
    make -j"$JOBS" platform=win CC="${CC:-x86_64-w64-mingw32-gcc}"
    ;;
  standalone)
    make -j"$JOBS" standalone
    ;;
  win-standalone)
    if [ -n "$2" ]; then
      SDLW=$2
    else
      SDLW=deps/sdl2-mingw/SDL2-$SDL2_VERSION
      if [ ! -d "$SDLW" ]; then
        mkdir -p deps/sdl2-mingw
        curl -fsSL -o deps/sdl2-mingw/sdl2.tar.gz \
          "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-devel-$SDL2_VERSION-mingw.tar.gz"
        tar xzf deps/sdl2-mingw/sdl2.tar.gz -C deps/sdl2-mingw
      fi
    fi
    P=$(cd "$SDLW/x86_64-w64-mingw32" && pwd)
    make -j"$JOBS" platform=win CC="${CC:-x86_64-w64-mingw32-gcc}" \
        OBJCOPY="${OBJCOPY:-x86_64-w64-mingw32-objcopy}" \
        SDL2_CONFIG="sh $P/bin/sdl2-config --prefix=$P" standalone
    ;;
  test)
    [ -n "$2" ] || { echo "usage: $0 test <game.pak>"; exit 1; }
    make -j"$JOBS"
    ${CC:-cc} -O1 -Wall -Ideps/libretro-common/include -o test/frontend test/frontend.c -ldl -lm
    OUT=${TMPDIR:-/tmp}/openbor-libretro-test
    rm -rf "$OUT"
    # wait for the title screen, press Start, then pick "Quit" (4x Down + Start)
    ./test/frontend ./openbor_libretro.so "$2" "$OUT" 4000 \
        500:0:START:6 600:0:DOWN 640:0:DOWN 680:0:DOWN 720:0:DOWN 780:0:START:6 \
        dump=450,560,700
    echo "Frames and audio written to $OUT"
    ;;
  *)
    make -j"$JOBS"
    ;;
esac
