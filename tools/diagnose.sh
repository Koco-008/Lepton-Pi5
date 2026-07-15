#!/bin/sh
set -eu

echo "== time =="
date -Is

echo "== uname =="
uname -a

echo "== os-release =="
if [ -r /etc/os-release ]; then
	cat /etc/os-release
fi

echo "== kernel headers =="
ls -l "/lib/modules/$(uname -r)/build" 2>/dev/null || true

echo "== kernel config =="
config=""
if [ -r "/boot/config-$(uname -r)" ]; then
	config="/boot/config-$(uname -r)"
elif [ -r /proc/config.gz ]; then
	zcat /proc/config.gz | grep -E 'CONFIG_(SPI|DMA|VIDEO|VIDEOBUF2)' || true
fi
if [ -n "$config" ]; then
	grep -E 'CONFIG_(SPI|DMA|VIDEO|VIDEOBUF2)' "$config" || true
fi

echo "== spi masters =="
ls -l /sys/class/spi_master 2>/dev/null || true

echo "== spi devices =="
ls -l /sys/bus/spi/devices 2>/dev/null || true

echo "== spi drivers =="
find /sys/bus/spi/devices -maxdepth 2 -type l -name driver -print -exec readlink -f {} \; 2>/dev/null || true

echo "== spi device tree paths =="
for dev in /sys/bus/spi/devices/*; do
	[ -e "$dev" ] || continue
	if [ -e "$dev/of_node" ]; then
		printf "%s -> " "$dev"
		readlink -f "$dev/of_node" || true
	fi
done

echo "== dev nodes =="
ls -l /dev/spidev* 2>/dev/null || true
ls -l /dev/video* 2>/dev/null || true

echo "== relevant modules =="
lsmod | grep -E '(^lepton|v4l2loopback|spi|videobuf2|v4l2|dw_spi)' || true

echo "== processed stream service =="
if command -v systemctl >/dev/null 2>&1; then
	systemctl --no-pager --full status lepton-streamer.service 2>/dev/null || true
fi

echo "== processed stream formats =="
if command -v v4l2-ctl >/dev/null 2>&1; then
	for dev in /dev/video10 /dev/video11; do
		if [ -c "$dev" ]; then
			echo "-- $dev"
			v4l2-ctl -d "$dev" --all 2>/dev/null || true
		fi
	done
fi

echo "== dtoverlay =="
if command -v dtoverlay >/dev/null 2>&1; then
	dtoverlay -l || true
else
	echo "dtoverlay not found"
fi

echo "== config.txt relevant lines =="
for cfg in /boot/firmware/config.txt /boot/config.txt; do
	if [ -r "$cfg" ]; then
		echo "-- $cfg"
		grep -E '^(dtparam|dtoverlay)=' "$cfg" || true
	fi
done

echo "== i2c lepton address =="
if command -v i2cdetect >/dev/null 2>&1; then
	i2cdetect -r -y 1 0x2a 0x2a || true
else
	echo "i2cdetect not found"
fi

echo "== userspace helpers =="
for helper in ./lepton_control/rpi_vsync_app ./lepton_data_collector/lepton_data_collector; do
	if [ -x "$helper" ]; then
		echo "$helper: present"
	else
		echo "$helper: missing or not executable"
	fi
done

echo "== lepton modinfo =="
if command -v modinfo >/dev/null 2>&1; then
	modinfo lepton 2>/dev/null || modinfo ./lepton_module/lepton.ko 2>/dev/null || true
fi

echo "== lepton sysfs counters =="
for dev in /sys/bus/spi/devices/*; do
	[ -e "$dev/driver" ] || continue
	driver=$(readlink -f "$dev/driver" || true)
	case "$driver" in
		*lepton*)
			echo "-- $dev"
			for attr in vsync_count spi_complete_count valid_subframe_count invalid_subframe_count sync_loss_count resync_count last_spi_status transfer_in_flight; do
				[ -r "$dev/$attr" ] && printf "%s=%s\n" "$attr" "$(cat "$dev/$attr")"
			done
			;;
	esac
done

echo "== recent kernel messages =="
dmesg --level=err,warn,notice,info 2>/dev/null | tail -n 200 || dmesg | tail -n 200 || true
