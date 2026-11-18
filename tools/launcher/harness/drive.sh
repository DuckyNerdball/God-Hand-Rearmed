#!/bin/bash
# Drive the launcher under Xvfb: mouse, keys, screenshots.
W=${W:-1600}; H=${H:-900}; OUT=/tmp/lh/shots; mkdir -p $OUT; rm -f $OUT/*.png
Xvfb :99 -screen 0 ${W}x${H}x24 >/dev/null 2>&1 & XP=$!
sleep 1
export DISPLAY=:99 LH_W=$W LH_H=$H
/tmp/lh/launcher > /tmp/lh/run.log 2>&1 & LP=$!
eval $(python3 "$(dirname "$0")/coords.py" $W $H | awk '{print toupper($1)"X="$2"; "toupper($1)"Y="$3";"}')
shot() { import -window root $OUT/$1.png; echo "shot $1"; }
click() { xdotool mousemove $1 $2; sleep 0.25; xdotool mousedown 1; sleep 0.25; xdotool mouseup 1; }
key() { xdotool keydown $1; sleep 0.25; xdotool keyup $1; }
for i in $(seq 1 120); do grep -q "saloon zoom ready" /tmp/lh/run.log && break; sleep 1; done
sleep 3
shot 01_home
xdotool mousemove $QUITX $QUITY; sleep 0.6; shot 02_hover_quit
xdotool mousemove $BOOKX $BOOKY; sleep 1.0; shot 03_hover_book
click $BANANASX $BANANASY; sleep 0.05; shot 04_bite; sleep 1.2
click $SMOOTHIEX $SMOOTHIEY; sleep 0.9; click $SMOOTHIEX $SMOOTHIEY; sleep 0.9; click $SMOOTHIEX $SMOOTHIEY; sleep 0.9; click $SMOOTHIEX $SMOOTHIEY; sleep 0.9; shot 05_sips
xdotool mousemove 800 50; sleep 0.3
key Down; sleep 0.3; key Down; sleep 0.4; shot 06_keys
click $OPTIONSX $OPTIONSY; sleep 0.1; shot 07_zooming; sleep 2.0; shot 08_options
key Escape; sleep 0.25; shot 09_leaving; sleep 2.5; shot 10_back_home
key F4; sleep 0.8; shot 11_tuning; key F4; sleep 0.3
xdotool mousemove $PLAYX $PLAYY; sleep 0.4; xdotool mousedown 1; sleep 0.25; xdotool mouseup 1; sleep 0.05; shot 12_play
sleep 2
kill $LP 2>/dev/null; kill $XP 2>/dev/null
tail -5 /tmp/lh/run.log
