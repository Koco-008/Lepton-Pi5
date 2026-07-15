# Lepton Raw and False-Color Video Streams

The Raspberry Pi 5 pipeline exposes two stable V4L2 capture devices for
applications. The kernel driver's `/dev/video0` node remains an internal VoSPI
transport and should not be opened by the GUI while the streamer is running.

| Device | Format | Purpose |
| --- | --- | --- |
| `/dev/video0` | 82x60 `Y16` | Internal raw VoSPI subframes with packet headers |
| `/dev/video10` | 160x120 `Y16` | Complete 16-bit raw image, little-endian, 38400 bytes |
| `/dev/video11` | 640x480 `YUYV` | Automatically scaled false-color webcam image |

The streamer validates packet IDs and segment order, assembles Lepton 3
segments 1 through 4, strips the four VoSPI header/CRC bytes from every line,
and corrects the SPI byte order. It publishes a frame only after all four
segments are valid.

## False-Color Mapping

Every completed 160x120 frame is scaled independently:

- the lowest raw value in the frame is blue;
- values then pass through cyan, green, and yellow;
- the highest raw value in the frame is red.

The display stream is enlarged to 640x480 with nearest-neighbor scaling. This
keeps each Lepton detector pixel visually distinct and avoids inventing
intermediate measurements.

This is relative contrast scaling, not a temperature calibration. The same
raw value can have a different color in the next frame when the scene minimum
or maximum changes. Use `/dev/video10` whenever the GUI needs raw values or
temperature calculations.

## Build and Unit Test

Build the I2C helper, streamer, and deterministic frame-pipeline tests:

```sh
cd ~/Lepton-Pi5
make -C lepton_sdk
make -C lepton_control
make -C lepton_streamer clean
make -C lepton_streamer
make -C lepton_streamer check
```

The test creates synthetic big-endian VoSPI segments and verifies:

- correct segment ordering and resynchronization;
- correct 160x120 pixel placement;
- correct conversion to little-endian V4L2 `Y16`;
- exact blue and red palette endpoints;
- YUYV output generation and dimension validation.

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

## End-to-End Test

Once the camera produces valid VoSPI segments, capture one frame from each
public stream and verify its exact size:

```sh
tools/test_streams_rpi5.sh
```

The test waits up to 15 seconds for each stream. It fails rather than accepting
a stale, partial, or incorrectly sized frame.

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
array shape depends on the OpenCV/V4L2 build, so raw acquisition should be
validated in the final GUI environment before temperature processing is added.

## Invalid VoSPI Input

Discard packets such as `2fff`, `4fff`, and `5fff`, all-zero transfers, wrong
packet IDs, and out-of-order segments are rejected. They are never turned into
public video frames. Rejections are rate-limited in the system journal:

```sh
journalctl -u lepton-streamer.service -f
```

This means the service can be installed before the current hardware issue is
fixed, but `/dev/video10` and `/dev/video11` will not advance until valid
segments 1, 2, 3, and 4 are received.

Stop the service before unloading or manually testing the Lepton kernel module,
because the service intentionally keeps `/dev/video0` open:

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
