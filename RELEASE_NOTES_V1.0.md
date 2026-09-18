# Lepton Pi5 v1.0

Release candidate based on the `revised` branch.

## Installation

On the Raspberry Pi 5 with matching kernel headers installed:

```sh
sudo tools/install_v1_rpi5.sh --dry-run
sudo tools/install_v1_rpi5.sh
sudo reboot
```

After reboot verify the camera contract and public streams:

```sh
sudo /usr/local/libexec/lepton/rpi_recovery_app --status
tools/test_streams_rpi5.sh
```

## v1.0 hardening

- Safe kernel SPI teardown and monotonic capture timestamps/sequence numbers.
- Standards-compliant V4L2 OUTPUT buffer lifecycle.
- VoSPI CRC-16 validation and deterministic corruption testing.
- Bounded CCI/FFC polling and I2C block-buffer bounds.
- Hardened Linux I2C short-I/O and descriptor handling.
- Safer legacy collector input, CLI and output-file handling.
- Explicit raw/TLinear conversion contract and corrected legacy endianness docs.
- Improved systemd restart/hardening settings.
- Expanded CI and repaired VoSPI library tests.

The `/dev/video10` stream is the measurement source. `/dev/video11` is
display-only false color with per-frame scaling.
