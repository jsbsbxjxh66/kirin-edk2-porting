# Kirin 970 (HiSilicon) boot image assembly overrides.
#
# Replicates the structure of the stock Huawei/Honor Android boot image:
#   - header v1, page 2048, base 0x0
#   - kernel: BootShim + UEFI.fd, gzipped, with the compat device tree
#     appended after the gzip stream (this is where HiSilicon LK picks it up)
#   - no ramdisk
#   - kernel stored at its natural size (no padding)
#
# The default hook (Silicon/platform.sh.inc) writes offsets at their
# mkbootimg defaults (kernel 0x8000, ramdisk 0x1000000, page 4096), which
# HiSilicon LK does not accept.

function platform_build_kernel(){
	cat \
		"${ROOTDIR}/tools/BootShim/BootShim.bin" \
		"${WORKSPACE}/Build/${DEVICE}/${_MODE}_${TOOLCHAIN}/FV/${SOC_PLATFORM}_UEFI.fd" \
		> "${WORKSPACE}/Build/${DEVICE}/${_MODE}_${TOOLCHAIN}/FV/${SOC_PLATFORM}_UEFI.fd-bootshim" \
		||return "$?"
	gzip -c \
		< "${WORKSPACE}/Build/${DEVICE}/${_MODE}_${TOOLCHAIN}/FV/${SOC_PLATFORM}_UEFI.fd-bootshim" \
		> "${WORKSPACE}/uefi-${DEVICE}.img.gz" \
		||return "$?"
	cat \
		"${WORKSPACE}/uefi-${DEVICE}.img.gz" \
		"${ROOTDIR}/Platform/${VENDOR_NAME}/${SOC_PLATFORM_L}/FdtBlob_compat/${PLATFORM_NAME}.dtb" \
		> "${WORKSPACE}/uefi-${DEVICE}.img.gz-dtb" \
		||return "$?"

	local KERNEL_FILE="${WORKSPACE}/uefi-${DEVICE}-kernel"
	cp "${WORKSPACE}/uefi-${DEVICE}.img.gz-dtb" "${KERNEL_FILE}"
}

function platform_build_bootimg(){
	local BOOTIMG="${OUTDIR}/boot-${DEVICE}${EXT}.img"

	python3 "${ROOTDIR}/tools/mkbootimg.py" \
	--kernel "${WORKSPACE}/uefi-${DEVICE}-kernel" \
	--ramdisk /dev/null \
	--base "${BOOTIMG_BASE:-0x00000000}" \
	--kernel_offset "${BOOTIMG_KERNEL_OFFSET:-0x00000000}" \
	--ramdisk_offset "${BOOTIMG_RAMDISK_OFFSET:-0x00000000}" \
	--tags_offset "${BOOTIMG_TAGS_OFFSET:-0x00000000}" \
	--pagesize "${BOOTIMG_PAGESIZE:-2048}" \
	--os_version "${BOOTIMG_OS_VERSION}" \
	--os_patch_level "${BOOTIMG_OS_PATCH_LEVEL}" \
	--header_version "${BOOTIMG_HEADER_VERSION}" \
	-o "${BOOTIMG}" \
	||return "$?"

	# The shared mkbootimg zeroes ramdisk_addr/second_addr whenever the
	# matching file is empty, but HiSilicon LK expects base+offset like the
	# stock images. Restore those two header fields.
	python3 - "${BOOTIMG}" "${BOOTIMG_BASE:-0x00000000}" \
		"${BOOTIMG_RAMDISK_OFFSET:-0x00000000}" \
		"${BOOTIMG_SECOND_OFFSET:-0x00f00000}" <<'PYEOF' ||return "$?"
import struct, sys

path, base = sys.argv[1], int(sys.argv[2], 16)
ramdisk, second = int(sys.argv[3], 16), int(sys.argv[4], 16)

with open(path, "r+b") as f:
    if f.read(8) != b"ANDROID!":
        sys.exit(1)
    f.seek(20)
    f.write(struct.pack("<I", (base + ramdisk) & 0xFFFFFFFF))
    f.seek(28)
    f.write(struct.pack("<I", (base + second) & 0xFFFFFFFF))
PYEOF
}
