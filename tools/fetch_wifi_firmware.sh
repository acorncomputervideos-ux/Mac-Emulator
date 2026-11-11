#!/bin/sh
# Download the Raspberry Pi Wi-Fi chip firmware that Circle's WLAN driver
# loads from /firmware on the SD card (the same files and names as
# circle/addon/wlan/firmware/Makefile), into the folder given.
set -e
OUT=${1:?usage: fetch_wifi_firmware.sh OUTDIR}
COMMIT=c9d3ae6584ab79d19a4f94ccf701e888f9f87a53     # as tested with Circle
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

git -C "$TMP" init -q
git -C "$TMP" remote add origin https://github.com/RPi-Distro/firmware-nonfree.git
git -C "$TMP" sparse-checkout set --no-cone /debian/config/brcm80211/brcm /debian/config/brcm80211/cypress /debian/copyright
git -C "$TMP" fetch -q --depth 1 --filter=blob:none origin $COMMIT
git -C "$TMP" checkout -q FETCH_HEAD

B="$TMP/debian/config/brcm80211"
mkdir -p "$OUT"
cp -L "$B/cypress/cyfmac43430-sdio.bin"        "$OUT/brcmfmac43430-sdio.bin"
cp -L "$B/brcm/brcmfmac43430-sdio.txt"         "$OUT/brcmfmac43430-sdio.txt"
cp -L "$B/cypress/cyfmac43430-sdio.clm_blob"   "$OUT/brcmfmac43430-sdio.clm_blob"
for f in brcmfmac43436-sdio.bin brcmfmac43436-sdio.txt brcmfmac43436-sdio.clm_blob \
         brcmfmac43436s-sdio.bin brcmfmac43436s-sdio.txt \
         brcmfmac43456-sdio.bin brcmfmac43456-sdio.txt brcmfmac43456-sdio.clm_blob \
         brcmfmac43455-sdio.txt; do
    cp -L "$B/brcm/$f" "$OUT/$f"
done
cp -L "$B/cypress/cyfmac43455-sdio-minimal.bin" "$OUT/brcmfmac43455-sdio.bin"
cp -L "$B/cypress/cyfmac43455-sdio.clm_blob"    "$OUT/brcmfmac43455-sdio.clm_blob"
P5="brcmfmac43455-sdio.raspberrypi,5-model-b"
cp -L "$B/cypress/cyfmac43455-sdio-minimal.bin" "$OUT/$P5.bin"
cp -L "$B/brcm/brcmfmac43455-sdio.txt"          "$OUT/$P5.txt"
cp -L "$B/cypress/cyfmac43455-sdio.clm_blob"    "$OUT/$P5.clm_blob"
cp -L "$TMP/debian/copyright"                   "$OUT/LICENCE.firmware-nonfree"
echo "Wi-Fi firmware copied to $OUT"
