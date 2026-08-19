# KU5P FPGA target

This target boots a Rocket-based Chipyard SoC from the on-board SD card and
provides one UART plus the board's 1 GiB DDR4 memory.

## Build

From `fpga/`:

```sh
make SUB_PROJECT=ku5p CONFIG=RocketKu5pConfig bitstream
```

The KU5P flow emits both `Ku5pHarness.bit` and `Ku5pHarness.bin`. To build and
check the raw SPI configuration image explicitly, use:

```sh
make SUB_PROJECT=ku5p CONFIG=RocketKu5pConfig flash-bin
```

`Ku5pHarness.bin` is written next to the bitstream under the target's `obj/`
directory. Program it at offset `0x0` into the on-board 128-Mbit (16-MiB)
MT25QU128 configuration flash using the SPI x4 interface. If a bitstream
already exists without a matching BIN file, `flash-bin` converts that existing
bitstream directly and does not repeat synthesis, placement, or routing.

The generated target uses:

- `clk` (E18): 50 MHz system clock;
- `c0_sys_clk_{p,n}` (T24/U24): 100 MHz DDR4 reference clock;
- UART1 (G16/H16): 115200 baud by software convention;
- SD card in SPI mode (AE15/AD15/AF13/AF14);
- two MT40A256M16GE-075E devices as one 1 GiB, 32-bit DDR4 interface.

LED0 is a heartbeat. LED7 is active when DDR calibration has completed.

## SD image

The boot ROM reads a 512-byte image header from sector 34 and the payload from
sector 35 onward. Create the header and payload image with:

```sh
python3 fpga/src/main/resources/ku5p/sdboot/make_sd_image.py payload.bin --crc32
```

Write the resulting `payload.bin.sdimg` at sector 34 of an already partitioned
SD card. The helper prints the corresponding `dd` command. The default load
and entry address are both `0x80000000`.
