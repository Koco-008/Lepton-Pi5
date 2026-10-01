#!/bin/sh
set -eu

REPO="Koco-008/Lepton-Pi5"
VERSION="v1.0.0"
SOURCE_COMMIT="f031edf2c7b50b20653a1c4f16353255012cdf2c"
VALIDATED_KERNEL="6.18.34+rpt-rpi-2712"
ARCHIVE_URL="https://github.com/${REPO}/archive/${SOURCE_COMMIT}.tar.gz"

usage() {
    cat <<'USAGE'
Usage: sudo sh install-v1.0.0.sh [--reboot]

One-click installer for the hardware-validated Lepton Pi5 v1.0.0 release.

It:
  - verifies Raspberry Pi 5 + 64-bit ARM
  - installs normal build/diagnostic dependencies
  - installs exact matching kernel headers when available
  - downloads the exact v1.0.0 release commit
  - builds and installs the kernel module and userspace stack
  - configures the V4L2 streams and systemd service

Options:
  --reboot   Reboot automatically after a successful installation.
  -h, --help Show this help text.
USAGE
}

reboot_after=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --reboot)
            reboot_after=1
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

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root, for example:" >&2
    echo "  sudo sh install-v1.0.0.sh" >&2
    exit 1
fi

model=""
if [ -r /proc/device-tree/model ]; then
    model=$(tr -d '\000' < /proc/device-tree/model 2>/dev/null || true)
fi

case "$model" in
    *"Raspberry Pi 5"*) ;;
    *)
        echo "Unsupported hardware: ${model:-unknown}. This release targets Raspberry Pi 5." >&2
        exit 1
        ;;
esac

arch=$(uname -m)
if [ "$arch" != "aarch64" ]; then
    echo "Unsupported architecture: $arch. This installer expects 64-bit aarch64." >&2
    exit 1
fi

command -v apt-get >/dev/null 2>&1 || {
    echo "apt-get is required. This installer targets Debian/Raspberry Pi OS based systems." >&2
    exit 1
}

kernel=$(uname -r)
kdir="/lib/modules/$kernel/build"

echo "== Lepton Pi5 $VERSION installer =="
echo "Model:  $model"
echo "Kernel: $kernel"
echo

if [ "$kernel" != "$VALIDATED_KERNEL" ]; then
    echo "NOTE: v1.0.0 was hardware-validated on kernel $VALIDATED_KERNEL."
    echo "      Your kernel is $kernel; the installer will use only exact matching headers."
    echo
fi

echo "[1/5] Installing build and diagnostic dependencies..."
apt-get update
env DEBIAN_FRONTEND=noninteractive apt-get install -y \
    ca-certificates curl build-essential device-tree-compiler \
    i2c-tools v4l-utils python3

if [ ! -d "$kdir" ]; then
    echo "[2/5] Matching kernel headers are missing; trying linux-headers-$kernel..."
    if apt-cache show "linux-headers-$kernel" >/dev/null 2>&1; then
        env DEBIAN_FRONTEND=noninteractive apt-get install -y "linux-headers-$kernel"
    fi
fi

if [ ! -d "$kdir" ]; then
    cat >&2 <<EOF_HEADERS
Matching kernel headers are still missing:
  $kdir

The Lepton kernel module must be built against the exact running kernel ($kernel).
Install the exact matching Raspberry Pi kernel headers, reboot if the kernel was
updated, then run this installer again. Do not build against headers for a
different kernel version.
EOF_HEADERS
    exit 1
fi

echo "[2/5] Matching kernel headers found: $kdir"

tmpdir=$(mktemp -d /tmp/lepton-pi5-v1.XXXXXX)
cleanup() {
    rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

archive="$tmpdir/lepton-pi5-$VERSION.tar.gz"
echo "[3/5] Downloading $REPO $VERSION..."
curl --fail --location --proto '=https' --tlsv1.2 \
    "$ARCHIVE_URL" -o "$archive"

tar -xzf "$archive" -C "$tmpdir"
src_dir=$(find "$tmpdir" -mindepth 1 -maxdepth 1 -type d -name 'Lepton-Pi5-*' -print -quit)
[ -n "$src_dir" ] || {
    echo "Could not locate the extracted source tree." >&2
    exit 1
}
[ -x "$src_dir/tools/install_v1_rpi5.sh" ] || {
    echo "The v1.0.0 source does not contain tools/install_v1_rpi5.sh." >&2
    exit 1
}

echo "[4/5] Building and installing Lepton Pi5 $VERSION..."
cd "$src_dir"
./tools/install_v1_rpi5.sh

echo "[5/5] Installation completed successfully."
echo
echo "After reboot, verify with:"
echo "  sudo /usr/local/libexec/lepton/rpi_recovery_app --status"
echo "  systemctl status lepton-streamer.service --no-pager"
echo "  v4l2-ctl -d /dev/video10 --get-fmt-video"
echo "  v4l2-ctl -d /dev/video11 --get-fmt-video"
echo
echo "Expected public streams:"
echo "  /dev/video10  160x120 Y16, TLinear Kelvin x100"
echo "  /dev/video11  640x480 YUYV false color"

if [ "$reboot_after" -eq 1 ]; then
    echo "Rebooting now..."
    reboot
else
    echo
    echo "A reboot is required before using the installed driver."
    echo "Run: sudo reboot"
fi
