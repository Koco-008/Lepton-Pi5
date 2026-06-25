# FLIR Lepton 3.5 on Raspberry Pi 5

This branch ports the FLIR Lepton V4L2 kernel module for Raspberry Pi 5 with
the RP1 DesignWare SPI controller and kernel `6.18.34+rpt-rpi-2712`.

The module is built as an external kernel module. It does not require building
or replacing the Raspberry Pi kernel.

## Wiring

Preferred wiring:

| Lepton breakout | Raspberry Pi 5 |
| --- | --- |
| CS | CE0 / GPIO8 / physical pin 24 |
| MOSI | GPIO10 / physical pin 19 |
| MISO | GPIO9 / physical pin 21 |
| CLK | GPIO11 / physical pin 23 |
| VSYNC | GPIO17 / physical pin 11 |
| SDA | GPIO2 / physical pin 3 |
| SCL | GPIO3 / physical pin 5 |
| VIN | 3V3 |
| GND | GND |

The old experimental userspace reader used manual chip select on GPIO22
physical pin 15. This port does not use that wiring by default. Move Lepton CS
to CE0/GPIO8 before enabling the overlay.

Do not drive manual userspace CS and kernel SPI CS at the same time.

## Build

Install the normal build tools:

```sh
sudo apt update
sudo apt install -y build-essential device-tree-compiler i2c-tools v4l-utils
```

On Debian 13 / Raspberry Pi kernel 6.18, `raspberrypi-kernel-headers` may not be
available as a package name. That is fine if the matching header link already
exists. The expected header link for the task target is:

```sh
/lib/modules/6.18.34+rpt-rpi-2712/build
```

Check the active system:

```sh
ls -l /lib/modules/$(uname -r)/build
```

Do not install or build against headers for a different kernel.

Build the module and overlay:

```sh
cd lepton_module
make clean
make KDIR=/lib/modules/$(uname -r)/build
make overlay
modinfo ./lepton.ko
cd ..
```

Build the userspace VSYNC helper and collector:

```sh
make -C lepton_sdk clean
make -C lepton_control clean
make -C lepton_data_collector clean
make -C lepton_sdk
make -C lepton_control
make -C lepton_data_collector
```

The top-level Makefile defaults to the native compiler. Set `CROSS_COMPILE=`
only when intentionally cross-compiling.

If you pulled this branch before the 2026-06-25 build fix, update it first:

```sh
git pull --ff-only
```

## Pre-Install Checks

Check I2C before debugging SPI:

```sh
sudo modprobe i2c-dev
i2cdetect -r -y 1 0x2a 0x2a
```

The address `2a` must be visible.

Run the diagnostic script:

```sh
tools/diagnose.sh
```

## Manual Install

The helper supports dry runs:

```sh
tools/install_rpi5.sh --dry-run
```

When the dry run looks correct:

```sh
sudo tools/install_rpi5.sh
sudo reboot
```

After reboot, verify:

```sh
ls -l /dev/spidev*
ls -l /sys/bus/spi/devices/
find /sys/bus/spi/devices -maxdepth 2 -type l -name driver -print -exec readlink -f {} \;
dmesg --level=err,warn,notice,info | tail -n 200
```

Expected:

- `/dev/spidev0.0` is absent.
- `/dev/spidev0.1` can remain present.
- The Lepton node exists on SPI0 CS0.
- The Lepton module probes without Oops or DMA mapping errors.

Enable Lepton VSYNC over I2C before capture tests:

```sh
sudo ./lepton_control/rpi_vsync_app
```

Expected output includes `LEP_SetOemGpioMode result = 0`. If the helper is
missing, rebuild it from the repository root:

```sh
make -C lepton_sdk
make -C lepton_control
```

## Diagnostics

The driver exposes read-only counters on the SPI device sysfs directory:

- `vsync_count`
- `spi_complete_count`
- `valid_subframe_count`
- `invalid_subframe_count`
- `sync_loss_count`
- `resync_count`
- `last_spi_status`
- `transfer_in_flight`

Use `tools/diagnose.sh` to collect the common state without secrets.

## Capture Validation

Use the existing collector for full Lepton 3.x frames:

```sh
mkdir -p /tmp/capture
lepton_data_collector/lepton_data_collector -3 -c 50 -o /tmp/capture/frame_
ls -l /tmp/capture/frame_*.gray
```

Each full frame should be `38400` bytes.

Convert a raw frame to 16-bit PGM for inspection:

```sh
tools/raw_to_pgm.py /tmp/capture/frame_000000.gray --endian little --output /tmp/capture/frame_000000.pgm
```

The converter reports min/max only. It does not interpret temperatures.

## Uninstall

Dry run first:

```sh
tools/uninstall_rpi5.sh --dry-run
```

Then:

```sh
sudo tools/uninstall_rpi5.sh
sudo reboot
```

After reboot, `/dev/spidev0.0` should be back if the normal SPI configuration is
enabled.

## Kernel Updates

`lepton.ko` is built for one exact kernel. Rebuild it after every kernel update.
Do not introduce DKMS until the normal external-module port has passed repeated
capture and stress tests.
