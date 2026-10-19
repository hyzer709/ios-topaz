#!/bin/bash
# Redmi Note 8 (ginkgo, SM6125 "trinket") test build of the shim.
# Differences from build.sh (topaz): -DGINKGO (panel 1080x2340, no UFS probe,
# no topaz-only cpufreq/IMEM/PS_HOLD writes) and a boot image *v2* with the
# stock DTB, because the ginkgo ABL boots v2 images (stock boot.img is v2).
# Milestone 1 only: no xnu payload, just the shim screen and the gold hand-off.
set -euo pipefail
cd "$(dirname "$0")"
DTB=${DTB:-../dtb/boot_unpacked/dtb}
[ -f "$DTB" ] || { echo "error: stock DTB not found at $DTB (run tools/unpack_bootv2.py first)" >&2; exit 1; }
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-DGINKGO -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
size=$((16#$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
# Same layout as the stock ginkgo boot.img header (base 0, kernel 0x8000,
# tags 0x100, dtb 0x1f00000, page 4096).
${MKBOOTIMG:-mkbootimg} --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
	--dtb "$DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
	--os_version 10.0.0 --os_patch_level 2021-01 \
	--cmdline "console=null" -o out/shim-ginkgo.img
ls -l out/Image out/shim-ginkgo.img
