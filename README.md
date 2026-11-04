# AD4080 Zephyr sample

Captures bursts from an [EVAL-AD4080ARDZ](https://www.analog.com/en/resources/evaluation-hardware-and-software/evaluation-boards-kits/eval-ad4080ardz.html) plugged into the Arduino header of an [SDP-K1](https://www.analog.com/en/resources/evaluation-hardware-and-software/evaluation-boards-kits/sdp-k1.html) (`adi_sdp_k1/stm32f469xx`).

The driver registers the ADC with Zephyr's ADC API. `adc_read()` / `adc_read_dt()` arms the on-chip FIFO, lets the board's CNV oscillator fill it, then reads the samples back over the data SPI chip-select.

## Hardware

- SDP-K1 stacked with EVAL-AD4080ARDZ.
- Power the evaluation board from its 7 V to 12 V barrel jack. USB on the SDP-K1 does not power the ADC board.
- Differential input on the board SMA connectors.
- Console is the SDP-K1 DAPLink USB-C serial port, 115200 8N1.

The overlay follows the Analog Devices SDP-K1 firmware pinout:

| Arduino | STM32 | Function |
| --- | --- | --- |
| D13 / D12 / D11 | PB3 / PB4 / PA7 | SPI1 SCK / MISO / MOSI, mode 3 |
| D8 | PG11 | Configuration chip-select |
| D10 | PA15 | Data-interface chip-select |
| D4 | PG9 | Conversion-clock path enable |
| D3 | PD12 | GPIO3, FIFO full |
| D2 | PG7 | 40 MHz CNV oscillator |
| D1 / D0 | PA0 / PA1 | 20 MHz / 10 MHz oscillators, held off |
| D9 | PB15 | Analog front-end enable |

`sampling-frequency-mhz` in `app.overlay` selects 10, 20, or 40. The matching oscillator GPIO must be present.

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

The onboard DAPLink also accepts `build/zephyr/zephyr.hex` copied onto its USB mass-storage drive. Open the DAPLink CDC port at 115200 8N1 to see the sample output.

Each second the sample prints the first eight signed 20-bit codes of a 32-sample burst, with millivolts, and the average of the whole burst. The channel `vref` is 3000 mV, which is the evaluation board's reference. For a differential channel Zephyr converts with `raw * 3000 / 2^19`, so full scale is about ±3000 mV.

## What one `adc_read()` does

1. Writes the FIFO watermark to the number of samples (`1 + extra_samplings`).
2. Arms immediate-trigger FIFO mode and enables the selected CNV oscillator.
3. Waits until GPIO3 or the device-status FIFO-full bit asserts.
4. Stops the oscillator, clocks out one header byte plus 3 bytes per sample, and sign-extends each code to 32 bits.
5. Disables the FIFO again.

`interval_us` must stay 0. Sample spacing inside the burst is the CNV clock, not a software delay. A read accepts at most 256 samples (`AD4080_MAX_SAMPLES` in the driver). Resolution must be 20. The driver checks product ID `0x0050` at startup.

This path is the SPI FIFO capture used for an MCU host. It does not stream the 40 MSPS LVDS port.
