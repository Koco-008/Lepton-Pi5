#!/bin/sh
# Lepton-Pi5 v1.0.1: update ONLY the userspace streamer, never the kernel driver.
set -eu

usage() {
    echo "Usage: sh tools/update_streamer_v1_0_1.sh --check|--install|--rollback" >&2
    echo "Build first: make -C lepton_sdk && make -C lepton_streamer clean all check" >&2
}

[ "$#" -eq 1 ] || { usage; exit 2; }
action=$1
case "$action" in --check|--install|--rollback) ;; *) usage; exit 2;; esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(dirname "$script_dir")
source_binary="$repo_root/lepton_streamer/lepton_streamer"
target="/usr/local/bin/lepton_streamer"
backup_dir="/var/lib/lepton-pi5/backups/v1.0.1"
backup="$backup_dir/lepton_streamer.pre-v1.0.1"

if [ "$action" != "--rollback" ]; then
    [ -f "$source_binary" ] && [ -x "$source_binary" ] || {
        echo "Missing built streamer: $source_binary" >&2; exit 1;
    }
    for marker in /run/lock/spot-lepton/cci.lock 'dropping unverified frame'; do
        LC_ALL=C grep -aFq "$marker" "$source_binary" || {
            echo "Built binary lacks reviewed CCI marker: $marker" >&2; exit 1;
        }
    done
fi

if [ "$action" = "--check" ]; then
    [ -f "$target" ] || { echo "Installed streamer not found: $target" >&2; exit 1; }
    if cmp -s "$source_binary" "$target"; then
        echo "Installed streamer matches this v1.0.1 build."
    else
        echo "Installed streamer differs; no files changed."
    fi
    exit 0
fi

[ "$(id -u)" -eq 0 ] || { echo "Install/rollback requires root." >&2; exit 1; }
[ -f "$target" ] && [ ! -L "$target" ] || { echo "Target must be an existing regular non-symlink file." >&2; exit 1; }
command -v systemctl >/dev/null 2>&1 || { echo "systemctl required." >&2; exit 1; }
command -v fuser >/dev/null 2>&1 || { echo "fuser required (psmisc)." >&2; exit 1; }
[ -c /dev/video10 ] && [ -c /dev/video11 ] || { echo "Expected V4L2 loopback devices missing." >&2; exit 1; }

# Stop the SPOT GUI AND streamer deliberately before invoking this utility.
# The utility NEVER stops/starts services, reloads modules or reboots.
if systemctl is-active --quiet spot-os-session.service; then
    echo "Stop SPOT OS session before replacing the streamer." >&2
    exit 1
fi
if systemctl is-active --quiet lepton-streamer.service; then
    echo "Stop lepton-streamer.service before replacing its binary." >&2
    exit 1
fi
if fuser -s /dev/video10 /dev/video11; then
    echo "A video device is in use; refusing to replace the binary." >&2
    exit 1
fi

install -d -m 0700 "$backup_dir"
if [ "$action" = "--install" ]; then
    if cmp -s "$source_binary" "$target"; then
        echo "Already installed; no changes."
        exit 0
    fi
    if [ -e "$backup" ]; then
        if ! cmp -s "$target" "$backup"; then
            echo "Existing backup differs from current streamer; refusing ambiguous reinstall." >&2
            exit 1
        fi
    else
        install -m 0755 "$target" "$backup"
    fi
    replacement=$source_binary
else
    [ -f "$backup" ] && [ ! -L "$backup" ] || {
        echo "No saved pre-v1.0.1 binary; cannot roll back." >&2; exit 1;
    }
    replacement=$backup
fi

tmp="$target.v1.0.1.$$"
trap 'rm -f "$tmp"' EXIT HUP INT TERM
install -m 0755 -o root -g root "$replacement" "$tmp"
mv -f "$tmp" "$target"
trap - EXIT HUP INT TERM
echo "Stream binary $( [ "$action" = "--rollback" ] && echo restored || echo installed ); services remain stopped."
echo "Start lepton-streamer.service BEFORE spot-os-session.service."
echo "Backup retained at: $backup"
