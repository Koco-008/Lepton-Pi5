# Lepton-Pi5 v1.0.1 — CCI concurrency and streamer reliability

Release date: 2026-10-22. Compatible with existing Raspberry Pi 5 / Lepton 3.5 / Debian 13 installation from v1.0.0. This patch changes **only** the userspace streamer's handling of the shared CCI bus. It does not change the kernel module, firmware, kernel headers, device tree, video loopback setup, driver configuration, or SPOT application.

## Fixes

- Serialize each `LEP_GetSysFFCStatus` transaction with the existing `/run/lock/spot-lepton/cci.lock` used by the SPOT app.
- Never publish a radiometric frame if the CCI lock is busy, the sensor reports non-READY status, or the status read fails.
- Avoid exiting on a single transient CCI status error; bound error logging and retain the existing eight-second valid-frame stall watchdog for recovery.
- Fail closed if an existing lock cannot be opened, while retaining standalone compatibility only if no shared-lock file exists.
- Add unit tests for read failures, non-READY status and real cross-process `flock` contention.

## Validated

- GitHub Actions userspace build and radiometric integration workflows passed on reviewed commit `20be84ea89c030822159ba70934bb66c3a1d45e6`.
- Native Raspberry Pi 5 (aarch64, Debian 13, kernel `6.18.50+rpt-rpi-2712`): strict C11 `-Werror` build, pipeline and CCI guard tests passed.
- Live SPOT 0.28.10 and SPOT OS 0.1.1 test: process PID 6531, 40m33s uninterrupted service, zero restarts, 21,112 published frames at about 8.7 FPS; no CCI status errors, stalls or crashes found in queried service journal.
- Neither the precise SmartFFC timing/thermal accuracy nor a clean cold boot has been fully validated by this test.

## Crucial V4L2 start-order constraint

The Lepton streamer must initialize loopback `/dev/video10` to `160x120 Y16` **before** the SPOT GUI opens it. During hardware testing, starting the GUI first allowed the loopback format to become `640x480 BGR3`, making the streamer fail immediately at `VIDIOC_S_FMT`. This integration ordering is being fixed separately in SPOT OS 0.1.2; it is **not** addressed by the Lepton userspace CCI patch.

## Updating an existing installation

**Do not rerun the v1.0.0 full installer** to apply this patch: it is pinned to the old source and also rebuilds/reinstalls more than the streamer. `tools/update_streamer_v1_0_1.sh` replaces *only* the prebuilt streamer, keeps a backup, and does not stop/start any systemd services.

1. Obtain the exact `v1.0.1` tag source, build `lepton_sdk` and `lepton_streamer`, then run `make -C lepton_streamer check`. Confirm the version tag before using a release build.
2. Stop ongoing SPOT recordings, updates, GUI session and streamer using the existing administered maintenance process. Confirm video devices are not held by other processes. **Keep SSH open.**
3. Inspect without mutation using `sh tools/update_streamer_v1_0_1.sh --check`, then install with `sudo sh tools/update_streamer_v1_0_1.sh --install`.
4. Start **the streamer first**, verify `/dev/video10` is `160x120 Y16`, then start SPOT OS session; check both services and live frames.
5. To revert, stop both services and verify video devices are free, then run `sudo sh tools/update_streamer_v1_0_1.sh --rollback`. Start streamer before GUI.

The updater refuses service/device conflicts and does not reboot, reload any kernel modules, remove lock files, or alter SPOT app. The rollback backup is stored at `/var/lib/lepton-pi5/backups/v1.0.1/lepton_streamer.pre-v1.0.1`. For the separately tested device where v1.0.1 code has already been installed manually, do **not** needlessly replace a running tested binary. Source release is the distribution artifact; an ARM binary should be compiled locally for the installed environment.

## Source and verification

The `v1.0.1` tag references the merge commit containing the fully reviewed CCI fix and release documentation. GitHub Actions must pass before the release is published. The release lists the tested pre-merge CCI commit explicitly to help auditors distinguish the exact on-device binary source from the later release documentation merge.