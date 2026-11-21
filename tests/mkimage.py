#!/usr/bin/env python3
"""Make a FAT32 card image from a card folder, for pwg-imgtest (which
reads it with the PicoCalc's own FAT code).  Needs pyfatfs
(pip install pyfatfs).

  tests/mkimage.py CARD_FOLDER card.img [SIZE_MB]
"""
import os
import sys

import fs.copy
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS


def main():
    src, img = sys.argv[1:3]
    size_mb = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    with open(img, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)
    pf = PyFat()
    pf.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT32, size=size_mb * 1024 * 1024, label="PICO")
    pf.close()
    dst = PyFatFS(img)
    fs.copy.copy_dir(fs.osfs.OSFS(src), "/", dst, "/")
    dst.close()
    print("%s: %s copied" % (img, ", ".join(sorted(os.listdir(src)))))


if __name__ == "__main__":
    import fs.osfs
    main()
