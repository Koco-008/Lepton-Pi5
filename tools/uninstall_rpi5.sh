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
config="/boot/firmware/config.txt"
module_dst="/lib/modules/$kernel/extra/lepton.ko"
overlay_dst="/boot/firmware/overlays/flir-lepton-rpi5.dtbo"
tmp_config="$config.lepton-remove.$$"

[ -f "$config" ] || { echo "Missing $config." >&2; exit 1; }

if [ "$dry_run" -eq 1 ]; then
	echo "DRY-RUN: remove dtoverlay=flir-lepton-rpi5 from $config"
else
	grep -v '^dtoverlay=flir-lepton-rpi5' "$config" > "$tmp_config"
	cp "$config" "$config.lepton-uninstall-backup-$(date +%Y%m%d-%H%M%S)"
	cat "$tmp_config" > "$config"
	rm -f "$tmp_config"
fi

if [ -e "$overlay_dst" ]; then
	run rm -f "$overlay_dst"
fi

if [ -e "$module_dst" ]; then
	run rm -f "$module_dst"
fi

run depmod -a "$kernel"

echo "Uninstall staged. Reboot, then verify /dev/spidev0.0 is restored."
