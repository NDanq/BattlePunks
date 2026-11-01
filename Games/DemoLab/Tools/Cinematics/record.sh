#!/bin/sh
# Records one cinematic as a video and a frame sheet (Tools/Cinematics/README.md):
#   Tools/Cinematics/record.sh C3 <out dir> [build config]      (from Games/DemoLab)
# The game plays it alone (-DemoLab.Cinematic) in a window, records it (-record, the
# Editor-target build's recorder) and quits; ffmpeg makes a sheet, a frame every second.
set -e
ID=$1
OUT=$2
CONFIG=${3:-RelWithDebInfo}
BIN=Binaries/Linux_$CONFIG
case $ID in
    C1|C2) LEVEL=L1_Foothills ;;
    C3|C4|C6) LEVEL=L2_Laboratory ;;
    C5) LEVEL=L3_Core ;;
    *) echo "usage: $0 C1..C6 <out dir> [config]"; exit 1 ;;
esac
mkdir -p "$OUT"
# -record takes a path under 128 bytes: a link beside the game to the output folder.
ln -sfn "$(cd "$OUT" && pwd)" "$BIN/CineOut"
(cd "$BIN" && PULSE_SERVER=${PULSE_SERVER:-unix:/nonexistent} timeout 150 ./DemoLab -Game.StartLevel=$LEVEL \
    -DemoLab.Cinematic=$ID -record=CineOut/$ID.mp4 > "CineOut/$ID.log" 2>&1) || true
rm -f "$BIN/CineOut"
ffmpeg -v error -y -i "$OUT/$ID.mp4" -vf "fps=1,scale=384:-1,tile=6x5" -frames:v 1 "$OUT/${ID}_sheet.jpg"
grep -E "cinematic test|cutscene|AutoPlay|ERROR" "$OUT/$ID.log" | head -20
