#!/bin/bash
# $1 binary, $2 output png
$1 -e "$(dirname "$0")/render_test.sh" &
PID=$!
sleep ${WAIT:-1.2}
geom=$(hyprctl clients -j | jq -r --arg pid "$PID" '.[] | select(.pid==($pid|tonumber)) | "\(.at[0]),\(.at[1]) \(.size[0])x\(.size[1])"')
echo "geom: $geom"
grim -g "$geom" "$2"
wait $PID
