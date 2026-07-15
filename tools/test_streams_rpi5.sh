#!/bin/sh
set -eu

raw_device=${RAW_DEVICE:-/dev/video10}
color_device=${COLOR_DEVICE:-/dev/video11}
raw_bytes=38400
color_bytes=614400
temporary_directory=$(mktemp -d /tmp/lepton-stream-test.XXXXXX)

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
v4l2-ctl -d "$raw_device" --get-fmt-video

echo "== false-color format =="
v4l2-ctl -d "$color_device" --get-fmt-video

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
