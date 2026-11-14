#!/bin/sh
# Minimal pkg-config shim: FFmpeg only needs freetype2 in this cross build.
if [ "$1" = "--version" ]; then
  echo "0.29.2"
  exit 0
fi
for a in "$@"; do
  case "$a" in
    *harfbuzz*)
      case "$1" in
        --exists) exit 0 ;;
        --cflags) echo "-I${HB_PREFIX}/include/harfbuzz"; exit 0 ;;
        --libs) echo "-L${HB_PREFIX}/lib -lharfbuzz"; exit 0 ;;
        --modversion) echo "8.3.0"; exit 0 ;;
        --variable=*) echo "${HB_PREFIX}"; exit 0 ;;
      esac
      ;;
    *freetype2*)
      case "$1" in
        --exists) exit 0 ;;
        --cflags) echo "-I${FT_PREFIX}/include/freetype2"; exit 0 ;;
        --libs) echo "-L${FT_PREFIX}/lib -lfreetype"; exit 0 ;;
        --modversion) echo "26.1.20"; exit 0 ;;
        --variable=*) echo "${FT_PREFIX}"; exit 0 ;;
      esac
      ;;
  esac
done
exit 1
