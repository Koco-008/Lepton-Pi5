#!/bin/sh
set -eu

dry_run=0
install_packages=1

usage() {
	cat <<'EOF'
Usage: sudo tools/install_streams_rpi5.sh [--dry-run] [--skip-packages]

Install the Lepton frame streamer, two v4l2loopback devices, and its systemd
service. The streamer and rpi_vsync_app must be built first.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--dry-run)
			dry_run=1
			;;
		--skip-packages)
			install_packages=0
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown option: $1" >&2
			usage >&2
			exit 2
			;;
	esac
	shift
done

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

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(dirname "$script_dir")
kernel=$(uname -r)
streamer="$repo_root/lepton_streamer/lepton_streamer"
vsync_helper="$repo_root/lepton_control/rpi_vsync_app"
service_source="$repo_root/systemd/lepton-streamer.service"
modprobe_config="/etc/modprobe.d/lepton-streams.conf"
modules_load_config="/etc/modules-load.d/lepton-streams.conf"
service_destination="/etc/systemd/system/lepton-streamer.service"

[ -x "$streamer" ] || {
	echo "Missing $streamer. Run: make -C lepton_streamer" >&2
	exit 1
}
[ -x "$vsync_helper" ] || {
	echo "Missing $vsync_helper. Build lepton_sdk and lepton_control first." >&2
	exit 1
}
[ -f "$service_source" ] || {
	echo "Missing $service_source." >&2
	exit 1
}
[ -d "/lib/modules/$kernel/build" ] || {
	echo "Missing matching kernel headers at /lib/modules/$kernel/build" >&2
	exit 1
}

if ! modinfo v4l2loopback >/dev/null 2>&1; then
	if [ "$install_packages" -eq 0 ]; then
		echo "v4l2loopback is unavailable and --skip-packages was selected." >&2
		exit 1
	fi
	command -v apt-get >/dev/null 2>&1 || {
		echo "apt-get is unavailable; install v4l2loopback for $kernel manually." >&2
		exit 1
	}
	run apt-get update
	run env DEBIAN_FRONTEND=noninteractive apt-get install -y v4l2loopback-dkms
	if command -v dkms >/dev/null 2>&1; then
		run dkms autoinstall -k "$kernel"
	fi
fi

if [ "$dry_run" -eq 0 ] && ! modinfo v4l2loopback >/dev/null 2>&1; then
	echo "v4l2loopback was not built for $kernel. Inspect the DKMS build log." >&2
	exit 1
fi

if systemctl cat lepton-streamer.service >/dev/null 2>&1; then
	run systemctl stop lepton-streamer.service
fi
run install -D -m 0755 "$streamer" /usr/local/bin/lepton_streamer
run install -D -m 0755 "$vsync_helper" /usr/local/libexec/lepton/rpi_vsync_app
run install -D -m 0644 "$service_source" "$service_destination"

if [ "$dry_run" -eq 1 ]; then
	echo "DRY-RUN: write two-device v4l2loopback options to $modprobe_config"
	echo "DRY-RUN: write v4l2loopback to $modules_load_config"
else
	printf '%s\n' \
		'options v4l2loopback devices=2 video_nr=10,11 card_label="FLIR Lepton Raw","FLIR Lepton False Color" exclusive_caps=1,1 max_buffers=4' \
		> "$modprobe_config"
	printf '%s\n' 'v4l2loopback' > "$modules_load_config"
fi

if [ "$dry_run" -eq 1 ]; then
	echo "DRY-RUN: reload v4l2loopback with the configured devices"
else
	if lsmod | grep -q '^v4l2loopback '; then
		if ! modprobe -r v4l2loopback; then
			echo "Unable to unload v4l2loopback. Close applications using its video devices and retry." >&2
			exit 1
		fi
	fi
	modprobe v4l2loopback
	if command -v udevadm >/dev/null 2>&1; then
		udevadm settle
	fi
	for device in /dev/video10 /dev/video11; do
		[ -c "$device" ] || {
			echo "Expected loopback device $device was not created." >&2
			exit 1
		}
	done
fi

run systemctl daemon-reload
run systemctl enable --now lepton-streamer.service

echo "Installed Lepton streams:"
echo "  /dev/video10  160x120 Y16 raw pixels"
echo "  /dev/video11  640x480 YUYV automatic false color"
echo "Run tools/test_streams_rpi5.sh after valid VoSPI packets are available."
