#!/bin/sh
# Downloads a public SoundCloud playlist as MP3 and copies it to the SD card.
# Usage: tools/fetch_playlist.sh <playlist-url> [out-dir]
set -e
URL="${1:?usage: $0 <soundcloud-playlist-url> [out-dir]}"
OUT="${2:-/tmp/scdl}"
FFMPEG="${FFMPEG:-/tmp/ffmpeg-static/ffmpeg}"
SD="${SD:-/run/user/$(id -u)/gvfs/mtp:host=-_DBI_XJJ10026316542/1: SD Card}"
YTDLP="${YTDLP:-$HOME/.local/bin/yt-dlp}"

mkdir -p "$OUT"
"$YTDLP" --ffmpeg-location "$FFMPEG" \
    --extract-audio --audio-format mp3 --audio-quality 4 \
    --add-metadata --no-warnings --ignore-errors \
    --download-archive "$OUT/archive.txt" \
    -o "$OUT/%(playlist_index)02d - %(uploader)s - %(title)s.%(ext)s" \
    "$URL"

if [ -d "$SD/switch/scswitch/music" ]; then
    mkdir -p "$SD/switch/scswitch/music/zzzzz"
    cp -f "$OUT"/*.mp3 "$SD/switch/scswitch/music/zzzzz/" 2>/dev/null || true
    echo "copied to SD: switch/scswitch/music/zzzzz"
else
    echo "SD not mounted; files are in $OUT"
fi
