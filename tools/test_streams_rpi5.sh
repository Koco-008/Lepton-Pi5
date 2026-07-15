#!/bin/sh
set -eu

raw_device=${RAW_DEVICE:-/dev/video10}
color_device=${COLOR_DEVICE:-/dev/video11}
raw_bytes=38400
color_bytes=614400
temporary_directory=$(mktemp -d /tmp/lepton-stream-test.XXXXXX)
counter_directory=/sys/bus/spi/devices/spi0.0

cleanup() {
	rm -rf "$temporary_directory"
}
trap cleanup EXIT INT TERM

command -v v4l2-ctl >/dev/null 2>&1 || {
	echo "v4l2-ctl is required." >&2
	exit 1
}

for device in "$raw_device" "$color_device"; do
	[ -c "$device" ] || {
		echo "Missing video device: $device" >&2
		exit 1
	}
done

echo "== service =="
systemctl --no-pager --full status lepton-streamer.service || true

echo "== raw format =="
raw_format=$(v4l2-ctl -d "$raw_device" --get-fmt-video)
printf '%s\n' "$raw_format"
printf '%s\n' "$raw_format" |
	grep -Eq "Width/Height[[:space:]]*:[[:space:]]*160/120$" || {
	echo "Unexpected raw dimensions; expected 160x120." >&2
	exit 1
}
printf '%s\n' "$raw_format" | grep -Fq "'Y16 '" || {
	echo "Unexpected raw pixel format; expected Y16." >&2
	exit 1
}

echo "== false-color format =="
color_format=$(v4l2-ctl -d "$color_device" --get-fmt-video)
printf '%s\n' "$color_format"
printf '%s\n' "$color_format" |
	grep -Eq "Width/Height[[:space:]]*:[[:space:]]*640/480$" || {
	echo "Unexpected false-color dimensions; expected 640x480." >&2
	exit 1
}
printf '%s\n' "$color_format" | grep -Fq "'YUYV'" || {
	echo "Unexpected false-color pixel format; expected YUYV." >&2
	exit 1
}

if [ -r "$counter_directory/valid_subframe_count" ] &&
	[ "$(cat "$counter_directory/valid_subframe_count")" -eq 0 ]; then
	echo "FAIL: the Lepton driver has received no valid VoSPI subframes." >&2
	for attribute in vsync_count spi_complete_count valid_subframe_count \
		zero_segment_count invalid_subframe_count sync_loss_count resync_count \
		last_spi_status; do
		if [ -r "$counter_directory/$attribute" ]; then
			printf '%s=%s\n' "$attribute" "$(cat "$counter_directory/$attribute")" >&2
		fi
	done
	echo "No public frame can be captured until valid_subframe_count increases." >&2
	exit 1
fi

echo "== capture one raw frame =="
timeout 15s v4l2-ctl -d "$raw_device" \
	--stream-mmap=4 --stream-count=1 \
	--stream-to="$temporary_directory/raw.y16"

echo "== capture one false-color frame =="
timeout 15s v4l2-ctl -d "$color_device" \
	--stream-mmap=4 --stream-count=1 \
	--stream-to="$temporary_directory/color.yuyv"

actual_raw_bytes=$(stat -c %s "$temporary_directory/raw.y16")
actual_color_bytes=$(stat -c %s "$temporary_directory/color.yuyv")
[ "$actual_raw_bytes" -eq "$raw_bytes" ] || {
	echo "Raw frame size mismatch: expected $raw_bytes, got $actual_raw_bytes" >&2
	exit 1
}
[ "$actual_color_bytes" -eq "$color_bytes" ] || {
	echo "False-color frame size mismatch: expected $color_bytes, got $actual_color_bytes" >&2
	exit 1
}

echo "PASS: raw frame is $actual_raw_bytes bytes"
echo "PASS: false-color frame is $actual_color_bytes bytes"
