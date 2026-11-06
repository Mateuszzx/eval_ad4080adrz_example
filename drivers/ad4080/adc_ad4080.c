/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr ADC driver for the AD4080 on EVAL-AD4080ARDZ.
 *
 * The register port is SPI mode 3. SCLK idles high, the converter
 * changes SDO on the falling edge, and the MCU samples on the rising
 * edge. A frame is one read/write bit, a 15-bit address, and one data
 * byte.
 *
 * Conversions come from the board CNV oscillator and land in the one
 * 16384-sample FIFO. That memory is single-port, so a read stops the
 * oscillator, waits for FIFO full, and DMA-clocks the data chip-select:
 * one header byte, then big-endian sign-extended 24-bit samples. The
 * data clock is 10.5 MHz on one lane. One FIFO is gap-free at the CNV
 * rate. The next FIFO starts after this readout.
 */

#define DT_DRV_COMPAT adi_ad4080

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ad4080, CONFIG_ADC_LOG_LEVEL);

#define AD4080_REG_INTERFACE_CONFIG_A 0x00
#define AD4080_REG_INTERFACE_CONFIG_B 0x01
#define AD4080_REG_CHIP_TYPE           0x03
#define AD4080_REG_PRODUCT_ID_L        0x04
#define AD4080_REG_PRODUCT_ID_H        0x05
#define AD4080_REG_SCRATCH_PAD         0x0A
#define AD4080_REG_INTERFACE_STATUS_A  0x11
#define AD4080_REG_DEVICE_STATUS       0x14
#define AD4080_REG_DATA_INTF_CONFIG_A  0x15
#define AD4080_REG_GPIO_CONFIG_A       0x19
#define AD4080_REG_GPIO_CONFIG_C       0x1B
#define AD4080_REG_GENERAL_CONFIG      0x1C
#define AD4080_REG_FIFO_WATERMARK_L    0x1D
#define AD4080_REG_FIFO_WATERMARK_H    0x1E

#define AD4080_SW_RESET           (BIT(7) | BIT(0))
#define AD4080_ADDR_ASC           BIT(5)
#define AD4080_SDO_ENABLE         BIT(4)
#define AD4080_SINGLE_INST        BIT(7)
#define AD4080_SPI_READ           BIT(7)
#define AD4080_NOT_READY          BIT(7)
#define AD4080_FIFO_FULL          BIT(7)
#define AD4080_DATA_INTF_SPI      BIT(0)
/* 0 = one SPI lane (SDOB). 1 = four lanes, which this board does not wire. */
#define AD4080_SPI_QUAD_LANES     BIT(2)
#define AD4080_INTF_CHK_EN        BIT(4)
#define AD4080_TEST_PATTERN       0xAC5D6
#define AD4080_TEST_SAMPLES       4
#define AD4080_GPIO3_EN           BIT(3)
#define AD4080_GPIO3_SEL_MASK     GENMASK(7, 4)
#define AD4080_GPIO_SEL_FIFO_FULL 1U
#define AD4080_FIFO_MODE_MASK     GENMASK(1, 0)
#define AD4080_FIFO_MODE_OFF      0U
#define AD4080_FIFO_MODE_IMMEDIATE 1U

#define AD4080_CHIP_TYPE          0x07
#define AD4080_PRODUCT_ID         0x0050
#define AD4080_RESOLUTION         20
/* One full result FIFO. A capture of this length is contiguous at the CNV rate. */
#define AD4080_MAX_SAMPLES        16384
#define AD4080_SAMPLE_BYTES       3
#define AD4080_FIFO_HEADER_BYTES  1
#define AD4080_FIFO_TIMEOUT_MS    200
/*
 * SPI1 is on APB2 at 84 MHz. /2 (42 MHz) and /4 (21 MHz) are both under
 * the 50 MHz DCLK limit, and both sampled the sign bit early on this
 * board: half the words at 42 MHz, and the first word of every FIFO plus
 * a few later words at 21 MHz. tDDSDO is 9.6 ns. /8 is 10.5 MHz, half a
 * period is 48 ns. DMA holds that clock across the whole FIFO.
 */
#define AD4080_DATA_SPI_HZ        10500000U
/* Datasheet initialization time after the reset is started. */
#define AD4080_RESET_SETTLE_MS    1

struct ad4080_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec data_cs;
	struct gpio_dt_spec fifo_full;
	struct gpio_dt_spec cnv_clk_en;
	struct gpio_dt_spec osc40;
	struct gpio_dt_spec osc20;
	struct gpio_dt_spec osc10;
	struct gpio_dt_spec afe_en;
	uint8_t sampling_mhz;
};

struct ad4080_data {
	struct spi_dt_spec data_bus;
	const struct gpio_dt_spec *cnv_osc;
	struct k_mutex lock;
	bool ready;
	uint8_t raw[AD4080_FIFO_HEADER_BYTES +
		    (AD4080_MAX_SAMPLES * AD4080_SAMPLE_BYTES)];
};

static int ad4080_reg_write(const struct device *dev, uint16_t reg, uint8_t val)
{
	const struct ad4080_config *cfg = dev->config;
	uint8_t tx[3] = { FIELD_GET(GENMASK(15, 8), reg),
			  FIELD_GET(GENMASK(7, 0), reg), val };
	const struct spi_buf buf = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf_set set = { .buffers = &buf, .count = 1 };

	return spi_write_dt(&cfg->bus, &set);
}

static int ad4080_reg_read(const struct device *dev, uint16_t reg, uint8_t *val)
{
	const struct ad4080_config *cfg = dev->config;
	uint8_t tx[3] = { FIELD_GET(GENMASK(15, 8), reg) | AD4080_SPI_READ,
			  FIELD_GET(GENMASK(7, 0), reg), 0 };
	uint8_t rx[3];
	const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rx_buf = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
	int ret;

	ret = spi_transceive_dt(&cfg->bus, &tx_set, &rx_set);
	if (ret == 0) {
		*val = rx[2];
	}

	return ret;
}

static int ad4080_update_bits(const struct device *dev, uint16_t reg,
			      uint8_t mask, uint8_t val)
{
	uint8_t tmp;
	int ret;

	ret = ad4080_reg_read(dev, reg, &tmp);
	if (ret) {
		return ret;
	}

	tmp = (tmp & ~mask) | (val & mask);
	return ad4080_reg_write(dev, reg, tmp);
}

static int ad4080_soft_reset(const struct device *dev)
{
	uint8_t status;
	int ret;

	/*
	 * Both SW_RESET bits are armed by writing 1, and the reset starts
	 * only when a later write clears either of them. Configuration
	 * access during the following 300 µs is ignored and latches
	 * NOT_READY_ERR until that bit is written with 1.
	 */
	ret = ad4080_reg_write(dev, AD4080_REG_INTERFACE_CONFIG_A, AD4080_SW_RESET);
	if (ret) {
		return ret;
	}

	ret = ad4080_reg_write(dev, AD4080_REG_INTERFACE_CONFIG_A, AD4080_SDO_ENABLE);
	if (ret) {
		return ret;
	}

	k_msleep(AD4080_RESET_SETTLE_MS);

	ret = ad4080_reg_read(dev, AD4080_REG_INTERFACE_STATUS_A, &status);
	if (ret) {
		return ret;
	}

	if ((status & AD4080_NOT_READY) == 0) {
		return 0;
	}

	ret = ad4080_reg_write(dev, AD4080_REG_INTERFACE_STATUS_A, AD4080_NOT_READY);
	if (ret) {
		return ret;
	}

	ret = ad4080_reg_read(dev, AD4080_REG_INTERFACE_STATUS_A, &status);
	if (ret) {
		return ret;
	}

	if ((status & AD4080_NOT_READY) != 0) {
		LOG_ERR("software reset timed out, interface status 0x%02x", status);
		return -ETIMEDOUT;
	}

	return 0;
}

static int ad4080_set_clock(const struct device *dev, bool run)
{
	const struct ad4080_config *cfg = dev->config;
	struct ad4080_data *data = dev->data;
	int ret;

	if (cfg->osc20.port != NULL) {
		ret = gpio_pin_set_dt(&cfg->osc20, 0);
		if (ret) {
			return ret;
		}
	}
	if (cfg->osc10.port != NULL) {
		ret = gpio_pin_set_dt(&cfg->osc10, 0);
		if (ret) {
			return ret;
		}
	}

	ret = gpio_pin_set_dt(&cfg->osc40, 0);
	if (ret) {
		return ret;
	}

	if (!run) {
		return 0;
	}

	return gpio_pin_set_dt(data->cnv_osc, 1);
}

static int ad4080_configure_output(const struct gpio_dt_spec *gpio, int value)
{
	int ret;

	if (!gpio_is_ready_dt(gpio)) {
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(gpio, GPIO_OUTPUT);
	if (ret) {
		return ret;
	}

	return gpio_pin_set_dt(gpio, value);
}

static int ad4080_channel_setup(const struct device *dev,
				const struct adc_channel_cfg *channel_cfg)
{
	struct ad4080_data *data = dev->data;

	if (!data->ready) {
		return -ENODEV;
	}

	if (channel_cfg->channel_id != 0) {
		return -EINVAL;
	}

	if (!channel_cfg->differential || channel_cfg->gain != ADC_GAIN_1 ||
	    channel_cfg->reference != ADC_REF_EXTERNAL0 ||
	    channel_cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT) {
		return -EINVAL;
	}

	return 0;
}

static uint32_t ad4080_sample_count(const struct adc_sequence *sequence)
{
	uint32_t count = 1U;

	if (sequence->options != NULL) {
		count += sequence->options->extra_samplings;
	}

	return count;
}

static int ad4080_wait_fifo_full(const struct device *dev)
{
	const struct ad4080_config *cfg = dev->config;
	int64_t deadline = k_uptime_get() + AD4080_FIFO_TIMEOUT_MS;
	uint8_t status = 0;
	int ret;

	do {
		if (gpio_pin_get_dt(&cfg->fifo_full) > 0) {
			return 0;
		}

		ret = ad4080_reg_read(dev, AD4080_REG_DEVICE_STATUS, &status);
		if (ret) {
			return ret;
		}

		if ((status & AD4080_FIFO_FULL) != 0) {
			return 0;
		}

		k_busy_wait(20);
	} while (k_uptime_get() < deadline);

	LOG_ERR("FIFO full timeout, device status 0x%02x", status);
	return -ETIMEDOUT;
}

static int ad4080_read_fifo(const struct device *dev, int32_t *samples, uint32_t count)
{
	struct ad4080_data *data = dev->data;
	size_t nbytes = AD4080_FIFO_HEADER_BYTES + (count * AD4080_SAMPLE_BYTES);
	const struct spi_buf rx_buf = { .buf = data->raw, .len = nbytes };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
	int ret;

	memset(data->raw, 0, nbytes);

	ret = spi_transceive_dt(&data->data_bus, NULL, &rx_set);
	if (ret) {
		return ret;
	}

	for (uint32_t i = 0; i < count; i++) {
		uint32_t raw24 = sys_get_be24(&data->raw[AD4080_FIFO_HEADER_BYTES +
							 (i * AD4080_SAMPLE_BYTES)]);

		/* The FIFO value is already sign-extended from 20 bits to 24. */
		samples[i] = (int32_t)(raw24 << 8) >> 8;
	}

	return 0;
}

static int ad4080_capture(const struct device *dev, int32_t *samples, uint32_t count)
{
	int ret;

	ret = ad4080_set_clock(dev, false);
	if (ret) {
		return ret;
	}

	ret = ad4080_reg_write(dev, AD4080_REG_FIFO_WATERMARK_L, count & 0xff);
	if (ret) {
		return ret;
	}

	ret = ad4080_reg_write(dev, AD4080_REG_FIFO_WATERMARK_H, (count >> 8) & 0xff);
	if (ret) {
		return ret;
	}

	ret = ad4080_update_bits(dev, AD4080_REG_GENERAL_CONFIG, AD4080_FIFO_MODE_MASK,
				 AD4080_FIFO_MODE_OFF);
	if (ret) {
		return ret;
	}

	ret = ad4080_update_bits(dev, AD4080_REG_GENERAL_CONFIG, AD4080_FIFO_MODE_MASK,
				 AD4080_FIFO_MODE_IMMEDIATE);
	if (ret) {
		return ret;
	}

	ret = ad4080_set_clock(dev, true);
	if (ret) {
		return ret;
	}

	ret = ad4080_wait_fifo_full(dev);

	/*
	 * The FIFO is single-port. Stop CNV before reading so a new
	 * conversion cannot collide with the SPI data transfer.
	 */
	int clock_ret = ad4080_set_clock(dev, false);

	if (ret == 0) {
		ret = ad4080_read_fifo(dev, samples, count);
	}

	int fifo_ret = ad4080_update_bits(dev, AD4080_REG_GENERAL_CONFIG,
					  AD4080_FIFO_MODE_MASK, AD4080_FIFO_MODE_OFF);

	if (ret) {
		return ret;
	}
	if (clock_ret) {
		return clock_ret;
	}

	return fifo_ret;
}

/* 20-bit two's complement, as stored in the sign-extended 24-bit FIFO word. */
static int32_t ad4080_sign_extend_20(uint32_t code)
{
	return (int32_t)(code << 12) >> 12;
}

/*
 * INTF_CHK_EN replaces conversion results with 0xAC5D6. The register text
 * calls that an LVDS function; this still runs it on the single-lane SPI
 * FIFO, which is the interface this board can actually read.
 */
static int ad4080_check_data_interface(const struct device *dev)
{
	struct ad4080_data *data = dev->data;
	int32_t samples[AD4080_TEST_SAMPLES];
	int32_t expect = ad4080_sign_extend_20(AD4080_TEST_PATTERN);
	bool match = true;
	int ret;
	int clear_ret;

	ret = ad4080_update_bits(dev, AD4080_REG_DATA_INTF_CONFIG_A,
				 AD4080_INTF_CHK_EN, AD4080_INTF_CHK_EN);
	if (ret) {
		return ret;
	}

	ret = ad4080_capture(dev, samples, AD4080_TEST_SAMPLES);
	clear_ret = ad4080_update_bits(dev, AD4080_REG_DATA_INTF_CONFIG_A,
				       AD4080_INTF_CHK_EN, 0);
	if (clear_ret) {
		return clear_ret;
	}
	if (ret) {
		LOG_WRN("single-lane SPI test pattern capture failed: %d", ret);
		return 0;
	}

	LOG_HEXDUMP_INF(data->raw,
			AD4080_FIFO_HEADER_BYTES +
				(AD4080_TEST_SAMPLES * AD4080_SAMPLE_BYTES),
			"single-lane SPI FIFO");

	for (uint32_t i = 0; i < AD4080_TEST_SAMPLES; i++) {
		if (samples[i] != expect) {
			match = false;
		}
	}

	if (match) {
		LOG_INF("single-lane SPI test pattern 0xAC5D6");
		return 0;
	}

	LOG_WRN("single-lane SPI test pattern mismatch, codes %d %d %d %d, expected %d",
		samples[0], samples[1], samples[2], samples[3], expect);
	return 0;
}

static int ad4080_read(const struct device *dev, const struct adc_sequence *sequence)
{
	struct ad4080_data *data = dev->data;
	uint32_t count;
	int ret;

	if (!data->ready) {
		return -ENODEV;
	}

	if (sequence->channels != BIT(0) || sequence->resolution != AD4080_RESOLUTION ||
	    sequence->oversampling != 0 || sequence->buffer == NULL) {
		return -EINVAL;
	}

	if (sequence->options != NULL && sequence->options->interval_us != 0) {
		return -ENOTSUP;
	}

	count = ad4080_sample_count(sequence);
	if (count == 0 || count > AD4080_MAX_SAMPLES) {
		return -EINVAL;
	}

	if (sequence->buffer_size < count * sizeof(int32_t)) {
		return -ENOMEM;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = ad4080_capture(dev, sequence->buffer, count);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int ad4080_init(const struct device *dev)
{
	const struct ad4080_config *cfg = dev->config;
	struct ad4080_data *data = dev->data;
	uint8_t id_l;
	uint8_t id_h;
	uint8_t chip;
	uint8_t scratch;
	int ret;

	k_mutex_init(&data->lock);

	if (!spi_is_ready_dt(&cfg->bus) || !gpio_is_ready_dt(&cfg->data_cs)) {
		return -ENODEV;
	}

	data->data_bus = cfg->bus;
	data->data_bus.config.frequency = AD4080_DATA_SPI_HZ;
	data->data_bus.config.cs.gpio = cfg->data_cs;
	data->data_bus.config.cs.cs_is_gpio = true;

	if (!spi_is_ready_dt(&data->data_bus) || !gpio_is_ready_dt(&cfg->fifo_full)) {
		return -ENODEV;
	}

	/*
	 * The SPI controller only configures the config chip-select from
	 * devicetree. DCS is a second GPIO on the same bus, so it has to be
	 * an output here or the data transfer never frames and MISO stays
	 * high (every code reads as -1).
	 */
	ret = gpio_pin_configure_dt(&cfg->data_cs, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return ret;
	}

	ret = gpio_pin_configure_dt(&cfg->fifo_full, GPIO_INPUT);
	if (ret) {
		return ret;
	}

	switch (cfg->sampling_mhz) {
	case 40:
		data->cnv_osc = &cfg->osc40;
		break;
	case 20:
		if (cfg->osc20.port == NULL) {
			LOG_ERR("20 MHz oscillator GPIO is required");
			return -EINVAL;
		}
		data->cnv_osc = &cfg->osc20;
		break;
	case 10:
		if (cfg->osc10.port == NULL) {
			LOG_ERR("10 MHz oscillator GPIO is required");
			return -EINVAL;
		}
		data->cnv_osc = &cfg->osc10;
		break;
	default:
		return -EINVAL;
	}

	ret = ad4080_configure_output(&cfg->osc40, 0);
	if (ret) {
		return ret;
	}
	if (cfg->osc20.port != NULL) {
		ret = ad4080_configure_output(&cfg->osc20, 0);
		if (ret) {
			return ret;
		}
	}
	if (cfg->osc10.port != NULL) {
		ret = ad4080_configure_output(&cfg->osc10, 0);
		if (ret) {
			return ret;
		}
	}

	ret = ad4080_configure_output(&cfg->cnv_clk_en, 1);
	if (ret) {
		return ret;
	}

	if (cfg->afe_en.port != NULL) {
		ret = ad4080_configure_output(&cfg->afe_en, 1);
		if (ret) {
			return ret;
		}
	}

	ret = ad4080_reg_read(dev, AD4080_REG_CHIP_TYPE, &chip);
	if (ret) {
		return ret;
	}
	ret = ad4080_reg_read(dev, AD4080_REG_PRODUCT_ID_L, &id_l);
	if (ret) {
		return ret;
	}
	ret = ad4080_reg_read(dev, AD4080_REG_PRODUCT_ID_H, &id_h);
	if (ret) {
		return ret;
	}

	LOG_INF("chip type 0x%02x, product id 0x%04x", chip, ((uint16_t)id_h << 8) | id_l);

	if ((chip & 0x0f) != AD4080_CHIP_TYPE ||
	    (((uint16_t)id_h << 8) | id_l) != AD4080_PRODUCT_ID) {
		LOG_ERR("unexpected AD4080 identity");
		return -ENODEV;
	}

	ret = ad4080_soft_reset(dev);
	if (ret) {
		return ret;
	}

	ret = ad4080_reg_write(dev, AD4080_REG_INTERFACE_CONFIG_A,
			       AD4080_SDO_ENABLE | AD4080_ADDR_ASC);
	if (ret) {
		return ret;
	}

	ret = ad4080_reg_write(dev, AD4080_REG_INTERFACE_CONFIG_B, AD4080_SINGLE_INST);
	if (ret) {
		return ret;
	}

	ret = ad4080_reg_write(dev, AD4080_REG_SCRATCH_PAD, 0xa5);
	if (ret) {
		return ret;
	}
	ret = ad4080_reg_read(dev, AD4080_REG_SCRATCH_PAD, &scratch);
	if (ret) {
		return ret;
	}
	if (scratch != 0xa5) {
		LOG_ERR("scratchpad readback 0x%02x", scratch);
		return -EIO;
	}

	/* Bit 0 selects SPI. Bit 2 stays clear: one lane, not quad SPI. */
	ret = ad4080_update_bits(dev, AD4080_REG_DATA_INTF_CONFIG_A,
				 AD4080_DATA_INTF_SPI | AD4080_SPI_QUAD_LANES,
				 AD4080_DATA_INTF_SPI);
	if (ret) {
		return ret;
	}

	ret = ad4080_update_bits(dev, AD4080_REG_GPIO_CONFIG_A, AD4080_GPIO3_EN,
				 AD4080_GPIO3_EN);
	if (ret) {
		return ret;
	}

	ret = ad4080_update_bits(dev, AD4080_REG_GPIO_CONFIG_C, AD4080_GPIO3_SEL_MASK,
				 AD4080_GPIO_SEL_FIFO_FULL << 4);
	if (ret) {
		return ret;
	}

	ret = ad4080_check_data_interface(dev);
	if (ret) {
		return ret;
	}

	data->ready = true;
	LOG_INF("CNV clock %u MHz, data SPI %u Hz, FIFO capture", cfg->sampling_mhz,
		data->data_bus.config.frequency);
	return 0;
}

static DEVICE_API(adc, ad4080_api) = {
	.channel_setup = ad4080_channel_setup,
	.read = ad4080_read,
	.ref_internal = 0,
};

#define AD4080_GPIO_OR(inst, prop) GPIO_DT_SPEC_INST_GET_OR(inst, prop, { 0 })

#define AD4080_INIT(inst)                                                                  \
	static const struct ad4080_config ad4080_cfg_##inst = {                            \
		.bus = SPI_DT_SPEC_INST_GET(inst, SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8)), \
		.data_cs = GPIO_DT_SPEC_INST_GET(inst, data_cs_gpios),                     \
		.fifo_full = GPIO_DT_SPEC_INST_GET(inst, fifo_full_gpios),                 \
		.cnv_clk_en = GPIO_DT_SPEC_INST_GET(inst, cnv_clk_en_gpios),               \
		.osc40 = GPIO_DT_SPEC_INST_GET(inst, osc40_en_gpios),                      \
		.osc20 = AD4080_GPIO_OR(inst, osc20_en_gpios),                             \
		.osc10 = AD4080_GPIO_OR(inst, osc10_en_gpios),                             \
		.afe_en = AD4080_GPIO_OR(inst, afe_en_gpios),                              \
		.sampling_mhz = DT_INST_PROP(inst, sampling_frequency_mhz),                \
	};                                                                                 \
	static struct ad4080_data ad4080_data_##inst;                                      \
	DEVICE_DT_INST_DEFINE(inst, ad4080_init, NULL, &ad4080_data_##inst,                \
			      &ad4080_cfg_##inst, POST_KERNEL, CONFIG_ADC_INIT_PRIORITY,   \
			      &ad4080_api);

DT_INST_FOREACH_STATUS_OKAY(AD4080_INIT)
