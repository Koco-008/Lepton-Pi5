# Lepton Pi5 v1.0.0

First hardware-validated Raspberry Pi 5 release of the radiometric Lepton stack.

Validated on a Raspberry Pi 5 with a FLIR Lepton 3.5 and kernel
`6.18.34+rpt-rpi-2712`. The release build, installer, radiometry contract,
raw stream and false-color stream were exercised on real hardware.

## Installation

On the Raspberry Pi 5 with matching kernel headers installed:

```sh
tools/install_v1_rpi5.sh --dry-run
sudo tools/install_v1_rpi5.sh
sudo reboot
```

After reboot verify the camera contract and public streams:

```sh
sudo /usr/local/libexec/lepton/rpi_recovery_app --status
sudo tools/test_streams_rpi5.sh
```

## v1.0.0 hardening

- Raspberry Pi 5 RP1/DesignWare SPI support with a dedicated device-tree overlay.
- Safe kernel SPI teardown and monotonic capture timestamps/sequence numbers.
- Standards-compliant V4L2 OUTPUT buffer lifecycle.
- VoSPI CRC-16 validation and deterministic corruption testing.
- Bounded CCI/FFC polling and I2C block-buffer bounds.
- Hardened Linux I2C short-I/O and descriptor handling.
- Automatic Lepton recovery plus radiometry/TLinear verification.
- Safer legacy collector input, CLI and output-file handling.
- Explicit raw/TLinear conversion contract and corrected legacy endianness docs.
- Dedicated systemd service user, bounded restart policy and quieter release logging.
- Expanded CI covering the streamer, frame pipeline, legacy collector, VoSPI library,
  radiometry configuration, Python tooling and shell scripts.

## Public video streams

- `/dev/video10`: 160x120 little-endian `Y16`, TLinear Kelvin x100.
- `/dev/video11`: 640x480 `YUYV`, automatic false color for display.

`/dev/video10` is the measurement source. Convert to Celsius with:

```text
celsius = raw / 100.0 - 273.15
```

`/dev/video11` is display-only false color with per-frame scaling and must not
be used to reconstruct temperatures.
