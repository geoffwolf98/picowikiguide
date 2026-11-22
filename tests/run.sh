#!/bin/sh
# PicoWikiGuide tests.  Needs build/ (make) and a test card made from
# the ZIMs named below:
#   CARD=/path/to/card sh tests/run.sh
# where CARD holds CLIMATE (wikipedia_en_climate_change_mini_2024-06.zim
# from openzim's zim-testing-suite) and TOP100 (wikipedia_en_100_2026-08.zim
# with zimprep -n TOP100 --pictures all).
#
# The scripts run with a heap like the PicoCalc's (PWG_HEAP bytes: the
# firmware's heap, 0x20080000 minus the end of .bss; see README), on the
# card folder and, when pyfatfs is installed, again on a FAT32 image of
# it read with the PicoCalc's own file code (IMG=0 to skip).
set -e
CARD=${CARD:?set CARD to the test card folder}
export PWG_HEAP=${PWG_HEAP:-181528}
fail=0
run_scripts() {	# $1: program, $2: card or image, $3: label
	for s in tests/scripts/*.txt; do
		if [ "$s" = tests/scripts/pictures.txt ] && [ ! -f "$CARD/TOP100/TOP100.I00" ]; then
			echo "skip $3 $s (TOP100 on the card has no pictures)"; continue
		fi
		sh tests/reset-card.sh "$CARD"
		if [ "$3" = image ]; then
			python3 tests/mkimage.py "$CARD" "$2" 96 > /dev/null
		fi
		if $1 "$2" "$s" > /tmp/pwg-test.out; then
			echo "ok   $3 $s"
		else
			echo "FAIL $3 $s"; cat /tmp/pwg-test.out; fail=1
		fi
	done
}
run_scripts build/pwg-headless "$CARD" folder
if [ "${IMG:-1}" = 1 ] && python3 -c "import pyfatfs" 2> /dev/null; then
	run_scripts build/pwg-imgtest /tmp/pwg-test.img image
	rm -f /tmp/pwg-test.img
fi
sh tests/reset-card.sh "$CARD"
unset PWG_HEAP
build/dicttest > /tmp/pwg-dict.out && echo "ok   dictionary flash copy" ||
	{ echo "FAIL dictionary flash copy"; cat /tmp/pwg-dict.out; fail=1; }
build/pagecat --font-fingerprint > /tmp/pwg-font.out && echo "ok   font fingerprint" ||
	{ echo "FAIL font fingerprint"; cat /tmp/pwg-font.out; fail=1; }
for w in "$CARD"/*/; do
	n=$(basename "$w")
	if build/pagecat "$CARD/$n/$n" --check > /tmp/pwg-check.out; then
		echo "ok   pagecat --check $n"
	else
		echo "FAIL pagecat --check $n"; cat /tmp/pwg-check.out; fail=1
	fi
	if build/pagecat "$CARD/$n/$n" --index-check > /tmp/pwg-check.out; then
		echo "ok   pagecat --index-check $n"
	else
		echo "FAIL pagecat --index-check $n"; cat /tmp/pwg-check.out; fail=1
	fi
done
exit $fail
