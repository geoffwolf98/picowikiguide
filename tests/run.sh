#!/bin/sh
# PicoWikiGuide tests.  Needs build/ (make) and a test card made from
# the ZIMs named below:
#   CARD=/path/to/card sh tests/run.sh
# where CARD holds CLIMATE (wikipedia_en_climate_change_mini_2024-06.zim
# from openzim's zim-testing-suite) and TOP100 (wikipedia_en_100_2026-08.zim).
set -e
CARD=${CARD:?set CARD to the test card folder}
fail=0
for s in tests/scripts/*.txt; do
	sh tests/reset-card.sh "$CARD"
	if build/pwg-headless "$CARD" "$s" > /tmp/pwg-test.out; then
		echo "ok   $s"
	else
		echo "FAIL $s"; cat /tmp/pwg-test.out; fail=1
	fi
done
for w in "$CARD"/*/; do
	n=$(basename "$w")
	build/pagecat "$CARD/$n/$n" --check > /tmp/pwg-check.out || { echo "FAIL pagecat $n"; fail=1; }
	echo "ok   pagecat --check $n"
done
exit $fail
