#!/bin/sh
# Builds the calculator for BIOS, 64-bit UEFI and 32-bit UEFI, and packs all
# three into one bootable disk image (build/calc.img).
# Needs: nasm, mtools (mformat/mmd/mcopy).
set -eu
cd "$(dirname "$0")"
mkdir -p build

PART_START=2048                 # FAT partition starts at 1 MiB
PART_SECTORS=30720              # 15 MiB
TOTAL=$((PART_START + PART_SECTORS))

nasm -f bin -I src/ -DPART_START=$PART_START -DPART_SECTORS=$PART_SECTORS \
     -o build/bios.bin src/bios.asm
nasm -f bin -I src/ -DX64 -o build/BOOTX64.EFI  src/uefi.asm
nasm -f bin -I src/       -o build/BOOTIA32.EFI src/uefi.asm

# The boot sector loads the BIOS build with one single-track read.
size=$(wc -c < build/bios.bin)
if [ "$size" -gt $((18 * 512)) ]; then
    echo "bios.bin is too large ($size bytes)" >&2
    exit 1
fi

img=build/calc.img
rm -f "$img"
dd if=/dev/zero of="$img" bs=512 count=$TOTAL status=none
# FAT16 file system holding the UEFI builds under the standard fallback names
mformat -i "$img@@$((PART_START * 512))" -T $PART_SECTORS -h 64 -s 32 \
        -H $PART_START -c 4 -v CALC ::
mmd   -i "$img@@$((PART_START * 512))" ::/EFI ::/EFI/BOOT
mcopy -i "$img@@$((PART_START * 512))" build/BOOTX64.EFI build/BOOTIA32.EFI ::/EFI/BOOT/
# Boot sector (with partition table) + BIOS build go in front of the partition
dd if=build/bios.bin of="$img" conv=notrunc status=none

echo "Built:"
ls -l build/bios.bin build/BOOTX64.EFI build/BOOTIA32.EFI "$img"
