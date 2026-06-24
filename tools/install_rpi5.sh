#!/bin/sh
set -eu

dry_run=0
if [ "${1:-}" = "--dry-run" ]; then
	dry_run=1
fi

run() {
	if [ "$dry_run" -eq 1 ]; then
		printf 'DRY-RUN:'
		printf ' %s' "$@"
		printf '\n'
	else
		"$@"
	fi
}

need_root() {
	if [ "$dry_run" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
		echo "Run as root, or use --dry-run." >&2
		exit 1
	fi
}

need_root

kernel=$(uname -r)
module_src="lepton_module/lepton.ko"
overlay_src="lepton_module/flir-lepton-rpi5.dtbo"
module_dst="/lib/modules/$kernel/extra/lepton.ko"
overlay_dst="/boot/firmware/overlays/flir-lepton-rpi5.dtbo"
config="/boot/firmware/config.txt"

[ -f "$module_src" ] || { echo "Missing $module_src. Build the module first." >&2; exit 1; }
[ -f "$overlay_src" ] || { echo "Missing $overlay_src. Build the overlay first." >&2; exit 1; }
[ -f "$config" ] || { echo "Missing $config." >&2; exit 1; }

backup="$config.lepton-backup-$(date +%Y%m%d-%H%M%S)"

run mkdir -p "/lib/modules/$kernel/extra"
run cp "$module_src" "$module_dst"
run cp "$overlay_src" "$overlay_dst"
run cp "$config" "$backup"

if grep -q '^dtoverlay=flir-lepton-rpi5' "$config"; then
	echo "config.txt already contains dtoverlay=flir-lepton-rpi5"
else
	if [ "$dry_run" -eq 1 ]; then
		echo "DRY-RUN: append dtoverlay=flir-lepton-rpi5 to $config"
	else
		printf '\n# FLIR Lepton 3.5 on SPI0 CS0, VSYNC GPIO17\ndtoverlay=flir-lepton-rpi5\n' >> "$config"
	fi
fi

run depmod -a "$kernel"

echo "Install staged. Reboot, then run tools/diagnose.sh."
