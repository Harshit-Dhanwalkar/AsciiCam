#!/usr/bin/env bash
# Verify Linux nolibc build is fully self-contained:
#   1. build/webcam_ascii is statically linked (no PT_INTERP, no DT_NEEDED)
#   2. every plugin .so is dependency-free (no DT_NEEDED)
#   3. every undefined strong symbol in each plugin is in nl_dlopen's
#      resolve table (lib/nl_dlfcn.c)

set -euo pipefail

TARGET=build/webcam_ascii

[ -f "$TARGET" ] || { echo "FAIL: $TARGET not built"; exit 1; }

# binary is fully static
if readelf -lW "$TARGET" | grep -q INTERP; then
  echo "FAIL: $TARGET has PT_INTERP (dynamically linked)"; exit 1
fi
if readelf -d "$TARGET" 2>/dev/null | grep -q NEEDED; then
  echo "FAIL: $TARGET has DT_NEEDED entries"; exit 1
fi

# plugins
shopt -s nullglob
sos=(build/*.so)
shopt -u nullglob
if (( ${#sos[@]} == 0 )); then
  echo "FAIL: no plugins found in build/ (did 'make plugins' run?)"; exit 1
fi

for so in "${sos[@]}"; do
  if readelf -d "$so" | grep -q NEEDED; then
    echo "FAIL: $so has DT_NEEDED entries"; exit 1
  fi
  for sym in $(nm -u "$so" | awk '$1=="U"{print $2}' | sed 's/@.*//'); do
    if ! grep -q "\"$sym\"" lib/nl_dlfcn.c; then
      echo "FAIL: $so needs '$sym' which nl_dlopen cannot resolve"; exit 1
    fi
  done
done

echo "OK: static binary, dependency-free plugins, all plugin symbols resolvable."
