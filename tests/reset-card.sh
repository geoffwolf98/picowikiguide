#!/bin/sh
# zero the state, history and favourites files of a test card
c=$1
head -c 256 /dev/zero > "$c/PWGSTATE.DAT"
for f in "$c"/*/*.HIS; do head -c 65536 /dev/zero > "$f"; done
for f in "$c"/*/*.FAV; do head -c 32768 /dev/zero > "$f"; done
