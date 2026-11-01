#!/bin/sh
# Records one cinematic as a video and a frame sheet (Tools/Cinematics/README.md):
#   Tools/Cinematics/record.sh C3 <out dir> [build config]      (from Games/DemoLab)
# The game plays it alone (-DemoLab.Cinematic) in a window, records it (-record, the
# Editor-target build's recorder) and quits. The level load and the settling before the
# cutscene are cut off (the run log's times: "Recording" to "cutscene started"; KEEP_LEAD=1
# keeps them); ffmpeg makes a sheet, a frame every second.
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
RUNLOG=$(sed -n 's/.*Logging to \(.*\.log\).*/\1/p' "$OUT/$ID.log" | head -1)
if [ -z "$KEEP_LEAD" ] && [ -n "$RUNLOG" ] && [ -f "$BIN/$RUNLOG" ]; then
    # seconds from the recorder's start to the cutscene's (the run log stamps every line to the ms)
    LEAD=$(python3 -c '
import re, sys
t = {}
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"\S+ (\d+):(\d+):(\d+)\.(\d+) ", line)
    if not m:
        continue
    s = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000.0
    if "Recording " in line and "rec" not in t:
        t["rec"] = s
    if "cutscene started" in line:
        t["cut"] = s
print("%.2f" % max(0.0, t["cut"] - t["rec"] - 0.1) if len(t) == 2 else "")' "$BIN/$RUNLOG")
    if [ -n "$LEAD" ]; then
        mv "$OUT/$ID.mp4" "$OUT/${ID}_full.mp4"
        ffmpeg -v error -y -ss "$LEAD" -i "$OUT/${ID}_full.mp4" -c:v libx264 -crf 18 -preset medium -pix_fmt yuv420p \
            "$OUT/$ID.mp4"
        rm -f "$OUT/${ID}_full.mp4"
        echo "$ID: the first $LEAD s (load, settle) cut"
    fi
fi
ffmpeg -v error -y -i "$OUT/$ID.mp4" -vf "fps=1,scale=384:-1,tile=6x5" -frames:v 1 "$OUT/${ID}_sheet.jpg"
grep -E "cinematic test|cutscene|AutoPlay|ERROR" "$OUT/$ID.log" | head -20
