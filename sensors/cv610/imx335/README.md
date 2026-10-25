# Sony IMX335 plugin for Hi3516CV610

This is the source-built HiSilicon V5 sensor plugin used by the CV610 backend
when `sensor.sensorType` is `imx335`. It was developed and verified on a
four-lane Hi3516CV610 board whose IMX335 runs from the native 37.125 MHz MCLK.

Five fixed modes are supported, and the table index is the plugin's
`img_mode`:

| idx | geometry | rate | RAW | plugin enum |
|---|---|---|---|---|
| 0 | 2592x1944 | 30 | 12 | `IMX335_5M_30FPS_12BIT_LINEAR_MODE` |
| 1 | 1296x972 (2x2 binning) | 60 | 10 | `IMX335_60FPS_BINNING_MODE` |
| 2 | 1920x1080 (crop) | 90 | 10 | `IMX335_60FPS_CROPPED_1080P_MODE` |
| 3 | 2592x1944 (full) | 40 | 10 | `IMX335_60FPS_FULL_1944P_MODE` |
| 4 | 2592x1520 (crop) | 30 | 10 | `IMX335_1520P_10BIT_MODE` |

Every rate above is what the T6 crop timing actually reaches, not what the
enum name says: the `60FPS_*` prefixes are historical and modes 2 and 3 run
faster and slower respectively. `src/cv610_modes.c` is the authoritative list
of what the daemon advertises; keep the two in step when changing a rate.

`IMX335_CROP_FLEX_LINEAR_MODE` (5) is implemented in the driver but
deliberately not advertised: the daemon's validator cannot express an
arbitrary window yet.

Build it with the same public OpenHisilicon headers and OpenIPC ARMv7 musl
toolchain as the daemon:

```sh
make sensor-cv610 SOC_BUILD=cv610 \
  CV610_CC=/path/to/arm-openipc-linux-musleabi-gcc \
  CV610_SDK_INC=/path/to/openhisilicon
```

Install the result as `/usr/lib/sensors/libsns_imx335.so`. No proprietary SDK
source or binary is stored here; the build consumes the public sensor
interface and `sensor_common.c` from the external OpenHisilicon checkout.

## Provenance

The register sequences, the per-mode timing and the sharpness of the ISP seeds
come from the Hi3516CV610 T6 driver work, whose sensor source was upstreamed
to OpenIPC/openhisilicon as #270. This directory is the waybeam-side copy: it
adds the `pfn_mirror_flip` scaffold and keeps the mode table aligned with
`src/cv610_modes.c`.

The ISP seeds are a working set rather than a factory calibration. The 14-bit
black level is `0x320`, lens shading is enabled, and the CCM/gamma/DRC/bayer
noise-reduction blocks come from the vendor tuning that shipped with the
driver; the noise calibration model is the T6 one, because no vendor
calibration exists for this part. Expect a measured IQ pass to move them.

## What the plugin programs

`imx335_cfg.h` holds Sony's power-on sequence — the reserved analog/ADC trim,
the MIPI TX timing, the readout window and the HDR context linear mode needs
parked. It runs once, in standby, before the per-mode writes, and the
per-mode writes win where the two overlap.

`cmos_isp_set_image_mode()` takes the daemon's table index as its `sns_mode`
hint. That hint is what keeps modes 0 and 4 apart: both run at 30 fps, so a
frame-rate match alone cannot tell them apart.

## Orientation

`imx335_mirror_flip()` writes the two orientation bits, `0x304E` (mirror) and
`0x304F` (flip). It deliberately does not touch the readout-window flank
registers (`0x3081`/`0x3083`) that the SigmaStar reference nudges per
orientation. Treat it as implementation-complete but VERIFY-on-silicon:
measure geometry and Bayer phase with `image.mirror`/`image.flip` before
trusting it.

## Limitations

- Linear mode only; HDR/WDR is not wired.
- No sensor-side crop-window API beyond `IMX335_CROP_FLEX_LINEAR_MODE`, which
  the daemon does not expose.
- The mode table assumes the 37.125 MHz MCLK. That is the boot clock for this
  sensor, so it holds on any craft whose loader names `imx335`, but a craft
  that clocks the sensor differently will silently deliver a scaled rate.
