# Raspberry Pi 5 Kernel 6.18 Porting Notes

Base commit: `6f92303aa148ad1505622fbc9ebc6899a69be3e2`

Working branch: `rpi5-kernel-6.18`

Target kernel from the task brief: `6.18.34+rpt-rpi-2712`

## 2026-06-24 Source Port

The original module was inspected on a Windows Codex workstation. A real target
module build could not be performed here because the target Pi kernel headers
and `make` are not available in this environment. The blocked baseline evidence
is recorded in `buildlogs/baseline-2026-06-24-windows-host.txt`.

Changes made in this pass:

- `lepton_module/flir_lepton.c`
  - Removed direct SPI DMA mapping from the driver.
  - Removed manual `spi_message.is_dma_mapped` and `spi_transfer.rx_dma`.
  - Stopped using a V4L2 DMA buffer as the SPI receive buffer.
  - Added a persistent private SPI capture buffer allocated at probe time.
  - Switched the V4L2 queue to `vb2_vmalloc_memops`.
  - Switched private V4L2 buffers to `struct vb2_v4l2_buffer`.
  - Updated `strlcpy()` calls to `strscpy()`.
  - Fixed the `cap->card` copy size.
  - Replaced `q->min_buffers_needed` with `q->min_queued_buffers` and
    `q->min_reqbufs_allocation`.
  - Updated the SPI remove callback to return `void`.
  - Added transfer overlap protection with `transfer_in_flight`.
  - Added read-only sysfs diagnostics on the SPI device:
    `vsync_count`, `spi_complete_count`, `valid_subframe_count`,
    `invalid_subframe_count`, `sync_loss_count`, `last_spi_status`,
    `transfer_in_flight`.
  - Selected Lepton 2.x/3.x VoSPI dimensions from the Device Tree compatible
    data. The Pi 5 overlay binds as `flir,lepton3`.

- `lepton_module/Makefile`
  - Default external module build now uses `/lib/modules/$(uname -r)/build`.
  - Added an `overlay` target for `flir-lepton-rpi5.dtbo`.

- `lepton_module/flir-lepton-rpi5.dts`
  - New Raspberry Pi 5 overlay for SPI0 CS0, mode 3, 20 MHz, GPIO17 rising
    edge VSYNC.
  - Disables `spidev0.0` only.
  - Leaves `spidev0.1` untouched.

No probe, VSYNC, SPI, V4L2, collector, or stress-test success is claimed yet.
Those require the real Raspberry Pi 5 with the Lepton wired to CE0/GPIO8 and
VSYNC on GPIO17.

## Required Target-Pi Validation

Run these steps on the Raspberry Pi 5 before installing:

```sh
cd lepton_module
make clean
make KDIR=/lib/modules/$(uname -r)/build
make overlay
modinfo ./lepton.ko
```

Then review and save the full build log. Do not install the module or overlay
until the build is clean enough to explain every remaining warning.

## 2026-06-25 Target Build Feedback

First target build on `6.18.34+rpt-rpi-2712` reached `flir_lepton.o` and found:

- `struct vb2_queue` no longer has `num_buffers`; queue setup now enforces the
  local minimum only against `*nbuffers`.
- vb2 callbacks needed internal linkage to avoid `-Wmissing-prototypes`
  warnings.
- `vsync_count` is `u64`, so debug formats now use `%llu`.
- The standalone `dtc -@ -I dts` overlay build did not preprocess C-style
  `#include` directives; the Pi 5 overlay now uses numeric values for GPIO input
  function (`0`) and rising-edge IRQ (`1`) so it builds with plain `dtc`.

After that fix, the target Pi built `lepton.ko`, built `flir-lepton-rpi5.dtbo`,
and `modinfo ./lepton.ko` reported vermagic
`6.18.34+rpt-rpi-2712 SMP preempt mod_unload modversions aarch64`.

Remaining warning cleanup:

- `get_subframe_index_from_subframe()` and `get_subframe_index()` are local
  helpers in `lepton_vospi_funcs.c`; they are now `static` to remove
  `-Wmissing-prototypes` warnings.
