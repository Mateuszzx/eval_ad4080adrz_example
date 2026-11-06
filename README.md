# AD4080 Zephyr sample

Captures bursts from an [EVAL-AD4080ARDZ](https://www.analog.com/en/resources/evaluation-hardware-and-software/evaluation-boards-kits/eval-ad4080ardz.html) plugged into the Arduino header of an [SDP-K1](https://www.analog.com/en/resources/evaluation-hardware-and-software/evaluation-boards-kits/sdp-k1.html) (`adi_sdp_k1/stm32f469xx`).

The firmware fills the SDP-K1's 16 MB SDRAM with FIFO bursts, then sends that buffer on a USB CDC device (VID `1209`, PID `4080`). The DAPLink serial port is the boot log only.

The AD4080 has one 16,384-sample FIFO, and that memory is single-port. A conversion result and a SPI read cannot use it at the same time, so the conversion clock stops while the FIFO is clocked out. Samples inside one FIFO are gap-free at the CNV rate. The next FIFO starts only after that readout finishes.

## Hardware

- SDP-K1 stacked with EVAL-AD4080ARDZ.
- Power the evaluation board from its 7 V to 12 V barrel jack. USB on the SDP-K1 does not power the ADC board.
- Differential input on the board SMA connectors.
- Boot log is the SDP-K1 DAPLink USB-C serial port at 1 Mbit/s. Leave its DTR line alone; toggling it can reset the target.
- Sample data is the separate CDC device `1209:4080`.

The overlay follows the Analog Devices SDP-K1 firmware pinout:

| Arduino | STM32 | Function |
| --- | --- | --- |
| D13 / D12 / D11 | PB3 / PB4 / PA7 | SPI1 SCK / MISO / MOSI, mode 3 |
| D8 | PG11 | Configuration chip-select |
| D10 | PA15 | Data-interface chip-select |
| D4 | PG9 | Conversion-clock path enable |
| D3 | PD12 | GPIO3, FIFO full |
| D0 | PA1 | 10 MHz CNV oscillator, the one in use |
| D1 / D2 | PA0 / PG7 | 20 MHz / 40 MHz oscillators, held off |
| D9 | PB15 | Analog front-end enable |

`sampling-frequency-mhz` in `app.overlay` selects 10, 20, or 40. The matching oscillator GPIO must be present. The current overlay selects 10.

SPI is mode 3 on both chip-selects: SCLK idles high, the converter changes SDO on the falling edge, and the MCU samples on the rising edge. A register frame is a read/write bit, a 15-bit address, and one data byte. The FIFO data port uses the same edges. Its clock is 10.5 MHz (APB2 at 84 MHz divided by 8), and DMA holds that clock for the whole FIFO. 21 MHz and 42 MHz are under the converter's 50 MHz limit, and both sampled the sign bit early on this board: the data is valid 9.6 ns after the falling edge, and the STM32 still needs 2 ns of setup before the rising edge. One lane is wired (SDOB). Quad SPI and the LVDS port are not on this header.

The USB controller is configured for high speed. On this board the link enumerates at full speed, about 0.9 MB/s. A full SDRAM dump is about 18 seconds after the capture.

## Build and flash

The application is this repository: `CMakeLists.txt`, `prj.conf`, `app.overlay`, and `src/`. The CMake file adds `drivers/ad4080` as a module, so the ADC driver and its devicetree binding are picked up without an extra `-DZEPHYR_EXTRA_MODULES` argument. The board name is `adi_sdp_k1/stm32f469xx`.

Host tools are Docker and [just](https://github.com/casey/just). The image `ghcr.io/mateuszzx/zephyr-workspace:latest` supplies the toolchain (`/opt/toolchains/zephyr-sdk-1.0.1`). It does not contain the Zephyr source tree. `app/west.yml` pins Zephyr v4.5.0-rc1 and the STM32 HAL, and `just init` clones those into the Docker volume mounted at `/opt/zephyr-ws`. They are not copied into this repository. `just wipe-volumes` deletes that volume.

```sh
just up
just init
just build
```

`just build` compiles this directory for `adi_sdp_k1/stm32f469xx`, using the tree in `/opt/zephyr-ws`. On just 1.21 the board is a positional argument, so `just build board=adi_sdp_k1/stm32f469xx` sets the board name to that literal string.

The same steps with GNU make, which does accept `BOARD=`:

```sh
make up
make init
make build
```

Inside the container, `just build` is the same recipe.

Flash with a debug probe attached to the SDP-K1 10-pin header. `just flash` and `make flash` run `west flash -d build` in the container as root so the probe is visible:

```sh
just flash
```

The onboard DAPLink also accepts `build/zephyr/zephyr.hex` copied onto its USB mass-storage drive. After `west flash`, reset the core and resume it. The flash stub leaves the core halted, and resuming that stub does not start from the reset vector. The boot line on the DAPLink port is `AD4080 ready, FIFO 16384, SDRAM 5592405 samples at 10000000 Hz`.

## What a capture does

The host asks for a sample count. The firmware repeats the following until that many samples are packed into SDRAM at `0xC0000000`, then sends the buffer:

1. Write the FIFO watermark to the number of samples in this burst, at most 16,384.
2. Arm immediate-trigger FIFO mode and enable the selected CNV oscillator.
3. Wait until GPIO3 or the device-status FIFO-full bit asserts.
4. Stop the oscillator. DMA-clock the data chip-select: one header byte plus 3 bytes per sample. Sign-extend each big-endian 24-bit code to 32 bits.
5. Disable the FIFO, then pack the codes back to 24-bit little-endian in SDRAM.

`interval_us` stays 0. Sample spacing inside one FIFO is the CNV clock, not a software delay. At 10 MHz a full FIFO is 1.64 ms of conversions. Clocking it out is 37 ms of SPI bits, and the burst measures about 47 ms with the register setup and the copy into SDRAM. A read accepts at most 16,384 samples. Resolution must be 20. The driver checks product ID `0x0050` at startup.

16 MB holds 5,592,405 of these 24-bit samples. The channel `vref` is 3000 mV. Full scale of the 20-bit differential code is about ±3000 mV, so one LSB is `3000 / 2^19` mV.

## Host

From `host/`, with the CDC device connected:

```sh
uv run ad4080-capture capture.npy
```

With no count, that is one full SDRAM buffer. `-n` is the samples in one USB transfer, and `-N` splits a longer capture into SDRAM-sized transfers. A `.npy` file is little-endian int32. Any other suffix is raw little-endian int32. The record is FIFO bursts placed back to back. It is not one gap-free capture past 16,384 samples.
