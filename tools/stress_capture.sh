#!/bin/sh
set -eu

count="${1:-20}"
frames="${2:-50}"
outdir="${3:-/tmp/lepton-stress}"
collector="${COLLECTOR:-./lepton_data_collector/lepton_data_collector}"

[ -x "$collector" ] || { echo "Collector not executable: $collector" >&2; exit 1; }

mkdir -p "$outdir"
start_mem=$(awk '/MemAvailable/ { print $2 }' /proc/meminfo 2>/dev/null || echo unknown)

i=1
while [ "$i" -le "$count" ]; do
	run_dir="$outdir/run_$i"
	mkdir -p "$run_dir"
	echo "run=$i frames=$frames"
	"$collector" -3 -c "$frames" -o "$run_dir/frame_"
	find "$run_dir" -name 'frame_*.gray' -size 38400c | wc -l
	i=$((i + 1))
done

end_mem=$(awk '/MemAvailable/ { print $2 }' /proc/meminfo 2>/dev/null || echo unknown)

echo "MemAvailable_start_kB=$start_mem"
echo "MemAvailable_end_kB=$end_mem"
echo "Recent kernel warnings:"
dmesg --level=err,warn 2>/dev/null | tail -n 100 || dmesg | tail -n 100 || true
