#!/bin/sh
set -u

recovery_app=/usr/local/libexec/lepton/rpi_recovery_app
lock_file=/run/lepton-recovery.lock
stamp_file=/run/lepton-recovery.last
minimum_interval=30

log() {
	printf 'Lepton recovery: %s\n' "$*" >&2
}

# systemd exports these variables to ExecStopPost. Exit status 75 is reserved
# for a frame-stall detected by lepton_streamer.
if [ "${EXIT_CODE:-}" != "exited" ] || [ "${EXIT_STATUS:-}" != "75" ]; then
	exit 0
fi

exec 9>"$lock_file"
if ! flock -n 9; then
	log "another recovery attempt is already running"
	exit 0
fi

now=$(date +%s)
last=0
if [ -r "$stamp_file" ]; then
	read -r last < "$stamp_file" || last=0
fi
case "$last" in
	''|*[!0-9]*) last=0 ;;
esac
if [ "$last" -gt 0 ] && [ $((now - last)) -lt "$minimum_interval" ]; then
	remaining=$((minimum_interval - (now - last)))
	log "skipping recovery for another ${remaining}s to avoid a reboot loop"
	exit 0
fi

log "stopping VoSPI clocking by unloading the kernel module"
if lsmod | grep -q '^lepton '; then
	if ! modprobe -r lepton; then
		log "unable to unload lepton; another process may still own the input"
		exit 0
	fi
fi
date +%s > "$stamp_file"

# FLIR requires more than 185 ms with chip select deasserted and SCK idle to
# force VoSPI back to its synchronization state.
sleep 0.25

if [ -x "$recovery_app" ]; then
	log "issuing the camera OEM reboot command"
	if ! timeout 3s "$recovery_app" --reboot-only; then
		log "reboot command did not return cleanly; continuing with boot polling"
	fi
	sleep 1
	if ! timeout 8s "$recovery_app" --configure --boot-timeout-ms 6000; then
		log "camera did not become ready or VSYNC/TLinear could not be configured"
	fi
else
	log "missing recovery helper: $recovery_app"
fi

if ! modprobe lepton; then
	log "unable to reload the lepton kernel module"
	exit 0
fi
udevadm settle

attempt=0
while [ "$attempt" -lt 50 ]; do
	if [ -c /dev/lepton-vspi ]; then
		device=$(readlink -f /dev/lepton-vspi 2>/dev/null || true)
		name=$(basename "$device")
		if [ -r "/sys/class/video4linux/$name/name" ] &&
		   [ "$(cat "/sys/class/video4linux/$name/name")" = "lepton" ]; then
			log "input restored at /dev/lepton-vspi -> $device"
			exit 0
		fi
	fi
	attempt=$((attempt + 1))
	sleep 0.1
done

log "kernel module loaded, but /dev/lepton-vspi was not recreated"
exit 0
