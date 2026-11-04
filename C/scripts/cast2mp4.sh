#!/bin/sh
# cast2mp4.sh - convert an asciicam .cast recording to mp4 / mkv / webm / gif
#
# .cast file is asciinema v2 format (a JSON header line followed by
# [seconds, "o", "<terminal bytes>"] events). It is exact, tiny and plays
# natively with `asciinema play file.cast`, but it isn't a video container
# To get an mp4/mkv/webm we go via GIF using `agg` (or `asciinema gif`) and
# then `ffmpeg` for final encode
#
# Usage:
#   ./cast2mp4.sh recording.cast              # -> recording.mp4
#   ./cast2mp4.sh recording.cast out.mp4
#   ./cast2mp4.sh recording.cast clip.gif
#   ./cast2mp4.sh recording.cast clip.mkv
#
# Requires (any one of first column for cast->gif, then ffmpeg for video):
#   - agg        https://github.com/asciinema/agg        (cargo install agg)
#     OR
#   - asciinema  pip install asciinema                     (also plays .cast)
#   - ffmpeg     apt install ffmpeg / brew install ffmpeg

set -e

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
  echo "Usage: $0 input.cast [output.mp4|mkv|webm|mov|gif]" >&2
  exit 1
fi

input="$1"
case "$input" in
  *.cast) ;;
  *) echo "Warning: input '$input' does not end in .cast" >&2 ;;
esac

if [ ! -f "$input" ]; then
  echo "Error: input file '$input' not found" >&2
  exit 1
fi

if [ $# -eq 2 ]; then
  output="$2"
else
  base="${input%.cast}"
  output="${base}.mp4"
fi

# Lowercase extension for switch below
ext=$(printf '%s' "$output" | sed 's/.*\.//')
case "$ext" in
  mp4|mkv|webm|mov|gif) ;;
  *)
    echo "Error: unsupported output extension '.$ext'" >&2
    echo "  Supported: mp4, mkv, webm, mov, gif" >&2
    exit 4
    ;;
esac

tmp_gif=$(mktemp -t cast2mp4.XXXXXX.gif)
trap 'rm -f "$tmp_gif"' EXIT INT TERM HUP

# 1. .cast -> .gif
if command -v agg >/dev/null 2>&1; then
  echo "[cast2mp4] agg:   $input -> (tmp gif)"
  agg "$input" "$tmp_gif"
elif command -v asciinema >/dev/null 2>&1; then
  echo "[cast2mp4] asciinema gif: $input -> (tmp gif)"
  # asciinema >= 2.x has a `gif` subcommand; older versions used --gif on play
  if asciinema gif --help >/dev/null 2>&1; then
    asciinema gif "$input" -o "$tmp_gif"
  else
    asciinema play --gif "$input" >"$tmp_gif"
  fi
else
  echo "Error: neither 'agg' nor 'asciinema' is installed." >&2
  echo "  Install one of:" >&2
  echo "    cargo install agg          # recommended, faster" >&2
  echo "    pip install asciinema" >&2
  exit 2
fi

# 2. .gif -> final container
if [ "$ext" = "gif" ]; then
  mv "$tmp_gif" "$output"
  echo "[cast2mp4] done:  $output"
  exit 0
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "Error: ffmpeg not installed (required for .$ext output)." >&2
  echo "  The intermediate GIF is preserved at: $tmp_gif" >&2
  trap - EXIT  # don't delete gif user might want
  exit 3
fi

echo "[cast2mp4] ffmpeg: (tmp gif) -> $output"
# yuv420p ensures broad player compatibility; +faststart moves moov atom
# to front so file streams / previews in browsers and file managers
# Crank fps up to 30 so fast typers / motion in ascii don't drop frames
ffmpeg -y -i "$tmp_gif" \
  -vf "fps=30,format=yuv420p" \
  -c:v libx264 -preset slow -crf 18 \
  -movflags +faststart \
  "$output" >/dev/null 2>&1

echo "[cast2mp4] done:  $output"
