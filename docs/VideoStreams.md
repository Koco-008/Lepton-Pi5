# Lepton Raw and False-Color Video Streams

The Raspberry Pi 5 pipeline exposes two stable V4L2 capture devices for
applications. The kernel driver's stable `/dev/lepton-vspi` alias remains an
internal VoSPI transport and should not be opened by the GUI while the streamer
is running. Its underlying `/dev/videoX` number can change after a reboot.

| Device | Format | Purpose |
| --- | --- | --- |
| `/dev/lepton-vspi` | 82x60 `Y16` | Internal raw VoSPI subframes with packet headers |
| `/dev/video10` | 160x120 `Y16` | TLinear temperature image, little-endian Kelvin x100, 38400 bytes |
| `/dev/video11` | 640x480 `YUYV` | Automatically scaled false-color webcam image |

The streamer validates packet IDs and segment order, assembles Lepton 3
segments 1 through 4, strips the four VoSPI header/CRC bytes from every line,
and corrects the SPI byte order. It publishes a frame only after all four
segments are valid.

The loopback devices intentionally advertise capture and output capabilities
(`exclusive_caps=0`). Debian Trixie's v4l2loopback 0.15 can otherwise drop its
capture-only state before the first queued frame, making `VIDIOC_G_FMT` fail
with `EINVAL`. This compatibility mode keeps both public formats queryable by
OpenCV and `v4l2-ctl` while the camera is not yet producing valid data. The
streamer still does not enqueue placeholder raw data while the input is invalid.
When `v4l2-ctl` is installed, the installer verifies both capture views after
starting the service and fails if either format cannot be queried.

## Radiometric Temperature Contract

Before every streamer start, and again after an automatic camera recovery,
`rpi_recovery_app --configure` explicitly sets and reads back all four required
camera states:

- radiometry enabled;
- TLinear enabled;
- automatic TLinear resolution disabled;
- fixed TLinear resolution `0.01 K` (`LEP_RAD_RESOLUTION_0_01`).

The service does not start if this verification fails. Check the current camera
state with:

```sh
sudo /usr/local/libexec/lepton/rpi_recovery_app --status
```

The expected machine-readable fields are:

```text
gpio_mode=5
radiometry_enabled=1
tlinear_enabled=1
tlinear_auto_resolution=0
tlinear_resolution_kelvin=0.01
tlinear_scale=100
temperature_contract=kelvin_x100
```

`--status` exits with zero only when GPIO mode is VSYNC and the complete
Kelvin-x100 contract is active. It still prints the readable fields before a
nonzero exit so diagnostics can show the mismatching state.

This follows sections 4.8.9 through 4.8.11 of the official
[FLIR Lepton Software IDD Rev 303](https://flir.netx.net/file/asset/12411/original/attachment):
TLinear output is Kelvin multiplied by the selected resolution scale, and the
0.01-K mode uses a scale factor of 100.

Every `/dev/video10` pixel is therefore an unsigned 16-bit Kelvin value with a
scale factor of 100. Convert it without additional camera calibration:

```text
kelvin = raw / 100.0
celsius = raw / 100.0 - 273.15
```

For example, `30000` means `300.00 K`, or `26.85 degC`. The public frame
remains exactly 38400 bytes and little-endian, so existing capture code does not
need a device, size, or byte-order change.

TLinear supplies calibrated scene-temperature values, but measurement accuracy
still depends on the radiometric model. Emissivity, reflected background,
atmospheric transmission, distance, and any IR window must match the physical
measurement setup. The integration currently leaves those camera compensation
parameters at their existing values. Do not claim calibrated object-surface
accuracy for low-emissivity or reflected targets until those parameters and the
measurement method are validated.

## False-Color Mapping

Every completed 160x120 frame is scaled independently:

- the lowest raw value in the frame is blue;
- values then pass through cyan, green, and yellow;
- the highest raw value in the frame is red.

The display stream is enlarged to 640x480 with nearest-neighbor scaling. This
keeps each Lepton detector pixel visually distinct and avoids inventing
intermediate measurements.

This remains relative contrast scaling even though its source values are now
verified temperatures. The same absolute temperature can have a different color
in the next frame when the scene minimum or maximum changes. `/dev/video11`
must not be decoded back into temperatures; use `/dev/video10` for all numeric
measurements.

## Build and Unit Test

Build the I2C helper, streamer, and deterministic frame-pipeline tests:

```sh
cd ~/Lepton-Pi5
make -C lepton_sdk
make -C lepton_control
make -C lepton_streamer clean
make -C lepton_streamer
make check
```

The test creates synthetic big-endian VoSPI segments and verifies:

- correct segment ordering and resynchronization;
- correct 160x120 pixel placement;
- correct conversion to little-endian V4L2 `Y16`;
- exact blue and red palette endpoints;
- YUYV output generation and dimension validation.

The control test also verifies the complete TLinear configuration sequence,
idempotent reconfiguration, SDK-error handling, and read-back failure. The
Python test verifies little-endian raw-frame decoding and Kelvin-to-Celsius
conversion.

## Install

The installer adds `v4l2loopback` when necessary, creates video devices 10 and
11, installs the binaries, and starts `lepton-streamer.service`:

```sh
sudo tools/install_streams_rpi5.sh --dry-run
sudo tools/install_streams_rpi5.sh
```

It requires matching headers at `/lib/modules/$(uname -r)/build`. If the
distribution's `v4l2loopback-dkms` package cannot build for the active kernel,
the installer stops instead of enabling a partially working service.

Inspect the result:

```sh
systemctl --no-pager --full status lepton-streamer.service
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video10 --get-fmt-video
v4l2-ctl -d /dev/video11 --get-fmt-video
journalctl -u lepton-streamer.service -n 100 --no-pager
```

Expected public formats are `160x120 Y16` and `640x480 YUYV`.

## Automatic Recovery

The installed service enables an eight-second completed-frame watchdog. Normal
Lepton FFC shutter events are shorter than this threshold. If VoSPI transfers
continue but no complete frame can be assembled for eight seconds, the streamer
exits with status 75 and its `ExecStopPost` recovery performs this sequence:

1. unload the `lepton` kernel module so chip select and SCK are idle;
2. wait 250 ms, exceeding FLIR's 185 ms VoSPI resynchronization requirement;
3. issue the supported `LEP_RunOemReboot` command over CCI/I2C;
4. wait for the camera boot-ready bit, restore GPIO3 to VSYNC mode, and verify
   radiometry plus fixed 0.01-K TLinear output;
5. reload the kernel module and verify `/dev/lepton-vspi` before systemd restarts
   the streamer.

Recovery attempts are separated by at least 30 seconds to avoid a reboot loop.
Inspect them with:

```sh
journalctl -u lepton-streamer.service -g 'Lepton recovery' --no-pager
systemctl show lepton-streamer.service -p NRestarts -p ExecMainStatus
```

This mechanism restores service after a recoverable camera or VoSPI lockup. It
does not make an unstable supply acceptable for measurement equipment. A GPIO
mode that unexpectedly returns from VSYNC (`5`) to its power-on default (`0`)
indicates that the camera restarted. In that case, verify the supply directly at
the breakout under load, connector retention, common ground, and short SPI
wiring even when software recovery succeeds.

## End-to-End Test

Once the camera produces valid VoSPI segments, capture one frame from each
public stream and verify its exact size:

```sh
sudo tools/test_streams_rpi5.sh
```

The test first validates both exact public formats and checks the kernel driver's
`valid_subframe_count`. Before capture it also reads back GPIO, radiometry,
TLinear, automatic-resolution, and fixed-resolution state from the camera. It
stops with the SPI counters when no valid subframe exists, because no public
frame can exist yet. Otherwise it waits up to 15 seconds for each stream and
fails rather than accepting a stale, partial, incorrectly sized, or
non-radiometric frame.

## Python GUI

The false-color stream behaves like a conventional V4L2 webcam. OpenCV should
convert its standard YUYV data to BGR automatically:

```python
import cv2

camera = cv2.VideoCapture("/dev/video11", cv2.CAP_V4L2)
if not camera.isOpened():
    raise RuntimeError("Cannot open FLIR Lepton false-color stream")

ok, bgr_frame = camera.read()
if not ok:
    raise RuntimeError("No complete Lepton frame available")
```

The raw `/dev/video10` stream is 16-bit single-channel `Y16`. Its exact OpenCV
array shape depends on the OpenCV/V4L2 build, so normalize either a native
`uint16` frame or the two returned little-endian bytes before conversion:

```python
import cv2
import numpy as np

capture = cv2.VideoCapture("/dev/video10", cv2.CAP_V4L2)
capture.set(cv2.CAP_PROP_CONVERT_RGB, 0)
ok, frame = capture.read()
if not ok or frame is None:
    raise RuntimeError("No complete radiometric Lepton frame")

if frame.dtype == np.uint16 and frame.size == 160 * 120:
    raw = frame.reshape(120, 160)
elif frame.dtype == np.uint8:
    byte_view = frame.reshape(120, 160, 2)
    raw = byte_view[..., 0].astype(np.uint16) | (
        byte_view[..., 1].astype(np.uint16) << 8
    )
else:
    raise RuntimeError(f"Unexpected Y16 representation: {frame.dtype} {frame.shape}")

celsius = raw.astype(np.float32) * 0.01 - 273.15
center_celsius = float(celsius[60, 80])
```

Use `time.monotonic_ns()` when the GUI receives a complete frame; the public
V4L2 payload does not add camera timestamps or compensation metadata.

## Invalid VoSPI Input

Lepton 3.x normally emits eight packet-aligned, zero-numbered segments after
every unique four-segment frame. The streamer counts these as `skipped` and
does not publish or log them as errors.

Discard packets such as `2fff`, `4fff`, and `5fff`, all-zero transfers, wrong
packet IDs, and out-of-order nonzero segments are rejected. They are never
turned into public video frames. Rejections are rate-limited in the system
journal:

```sh
journalctl -u lepton-streamer.service -f
```

This means the service can be installed before the current hardware issue is
fixed. Their formats remain queryable, but reads wait and the devices do not
advance until valid segments 1, 2, 3, and 4 are received.

Stop the service before unloading or manually testing the Lepton kernel module,
because the service intentionally keeps `/dev/lepton-vspi` open:

```sh
sudo systemctl stop lepton-streamer.service
# perform manual driver tests
sudo systemctl start lepton-streamer.service
```

## Uninstall

Remove the service and its two loopback devices without removing the Lepton
kernel driver:

```sh
sudo tools/uninstall_streams_rpi5.sh --dry-run
sudo tools/uninstall_streams_rpi5.sh
```

The `v4l2loopback` package remains installed so uninstalling this feature does
not unexpectedly remove a shared kernel package.
