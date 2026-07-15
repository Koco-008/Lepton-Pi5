#!/bin/sh
set -eu

dry_run=0
if [ "${1:-}" = "--dry-run" ]; then
	dry_run=1
elif [ "$#" -ne 0 ]; then
	echo "Usage: sudo tools/uninstall_streams_rpi5.sh [--dry-run]" >&2
	exit 2
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

if [ "$dry_run" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
	echo "Run as root, or use --dry-run." >&2
	exit 1
fi

if systemctl cat lepton-streamer.service >/dev/null 2>&1; then
	run systemctl disable --now lepton-streamer.service
fi
run rm -f /etc/systemd/system/lepton-streamer.service
run rm -f /etc/modprobe.d/lepton-streams.conf
run rm -f /etc/modules-load.d/lepton-streams.conf
run rm -f /usr/local/bin/lepton_streamer
run rm -f /usr/local/libexec/lepton/rpi_vsync_app
run systemctl daemon-reload

if [ "$dry_run" -eq 1 ]; then
	echo "DRY-RUN: unload v4l2loopback when no process is using it"
elif lsmod | grep -q '^v4l2loopback '; then
	if ! modprobe -r v4l2loopback; then
		echo "v4l2loopback remains loaded because a process is using it." >&2
	fi
fi

echo "Lepton stream service removed. The v4l2loopback package was kept installed."
