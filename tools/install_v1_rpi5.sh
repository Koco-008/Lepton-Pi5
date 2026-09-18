#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(dirname "$script_dir")
dry_run=0

if [ "${1:-}" = "--dry-run" ]; then
	dry_run=1
elif [ "$#" -ne 0 ]; then
	echo "Usage: sudo tools/install_v1_rpi5.sh [--dry-run]" >&2
	exit 2
fi

if [ "$dry_run" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
	echo "Run as root, or use --dry-run." >&2
	exit 1
fi

kernel=$(uname -r)
kdir="/lib/modules/$kernel/build"
[ -d "$kdir" ] || {
	echo "Missing matching kernel headers: $kdir" >&2
	exit 1
}

echo "Building Lepton v1.0 components for $kernel..."
make -C "$repo_root/lepton_sdk" clean all
make -C "$repo_root/lepton_control" clean
make -C "$repo_root/lepton_control" rpi_vsync_app rpi_recovery_app
make -C "$repo_root/lepton_data_collector" clean all
make -C "$repo_root/lepton_streamer" clean all
make -C "$repo_root/lepton_streamer" check
make -C "$repo_root/lepton_vospi_lib" clean
make -C "$repo_root/lepton_vospi_lib"
"$repo_root/lepton_vospi_lib/test_lepton_driver_lib"
make -C "$repo_root/lepton_module" clean
make -C "$repo_root/lepton_module" rpi5 KDIR="$kdir"

if [ "$dry_run" -eq 1 ]; then
	"$repo_root/tools/install_rpi5.sh" --dry-run
	"$repo_root/tools/install_streams_rpi5.sh" --dry-run
	echo "Dry run complete; no system files were changed."
	exit 0
fi

"$repo_root/tools/install_rpi5.sh"
"$repo_root/tools/install_streams_rpi5.sh"

cat <<EOF
Lepton v1.0 installation staged successfully.
Kernel: $kernel
If the device-tree overlay changed, reboot before relying on the streams.
After reboot run:
  sudo /usr/local/libexec/lepton/rpi_recovery_app --status
  $repo_root/tools/test_streams_rpi5.sh
EOF
