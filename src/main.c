/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fill the SDP-K1 SDRAM with AD4080 FIFO bursts, then send that buffer
 * on the USB high-speed CDC port. Each burst is one FIFO deep and is
 * gap-free at the CNV rate. The SPI readout sits between bursts.
 *
 * Request:  0xA5 0x5A, uint32 little-endian count
 *           count 0 asks for limits instead of samples
 * Response: 0x5A 0xA5, uint32 count, int16 status, then the payload
 *           samples are count signed 24-bit little-endian codes
 *           a count-0 reply carries three int32 values:
 *           FIFO depth, samples that fit in SDRAM, sample rate
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define FIFO_SAMPLES 16384
#define SAMPLE_HZ (DT_PROP(DT_NODELABEL(ad4080), sampling_frequency_mhz) * 1000000)
#define SDRAM_ADDR DT_REG_ADDR(DT_NODELABEL(sdram1))
#define SDRAM_BYTES DT_REG_SIZE(DT_NODELABEL(sdram1))
#define RAM_SAMPLES (SDRAM_BYTES / 3)

#define RX_RING 128

const struct device *usb_serial_start(void);

static const struct device *usb;
static const struct adc_dt_spec adc_channel = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static int32_t samples[FIFO_SAMPLES];

static struct adc_sequence_options options;
static struct adc_sequence sequence = {
	.buffer = samples,
	.options = &options,
};

static uint8_t rx_ring[RX_RING];
static unsigned int rx_w;
static unsigned int rx_r;

static const uint8_t *tx_data;
static size_t tx_len;
static size_t tx_off;
static atomic_t tx_active;
static K_SEM_DEFINE(tx_done, 0, 1);

static void rx_put(const uint8_t *data, int len)
{
	unsigned int key = irq_lock();

	for (int i = 0; i < len; i++) {
		unsigned int next = (rx_w + 1U) % RX_RING;

		if (next == rx_r) {
			break;
		}
		rx_ring[rx_w] = data[i];
		rx_w = next;
	}
	irq_unlock(key);
}

static int rx_get(uint8_t *byte)
{
	unsigned int key = irq_lock();

	if (rx_r == rx_w) {
		irq_unlock(key);
		return -1;
	}
	*byte = rx_ring[rx_r];
	rx_r = (rx_r + 1U) % RX_RING;
	irq_unlock(key);
	return 0;
}

static void usb_uart_cb(const struct device *dev, void *user_data)
{
	uint8_t tmp[64];

	ARG_UNUSED(user_data);
	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev) > 0) {
		int n = uart_fifo_read(dev, tmp, sizeof(tmp));

		if (n <= 0) {
			break;
		}
		rx_put(tmp, n);
	}

	if (!atomic_get(&tx_active)) {
		return;
	}

	while (uart_irq_tx_ready(dev) > 0) {
		size_t left = tx_len - tx_off;
		int n;

		if (left == 0) {
			uart_irq_tx_disable(dev);
			if (atomic_cas(&tx_active, 1, 0)) {
				k_sem_give(&tx_done);
			}
			return;
		}

		n = uart_fifo_fill(dev, &tx_data[tx_off], left);
		if (n <= 0) {
			break;
		}
		tx_off += n;
	}
}

static bool host_is_open(void)
{
	uint32_t dtr = 0;

	if (uart_line_ctrl_get(usb, UART_LINE_CTRL_DTR, &dtr) != 0) {
		return true;
	}
	return dtr != 0;
}

static void stop_tx(void)
{
	if (atomic_cas(&tx_active, 1, 0)) {
		uart_irq_tx_disable(usb);
	}
}

static int send_bytes(const uint8_t *data, size_t len)
{
	int64_t deadline;

	if (len == 0) {
		return 0;
	}

	tx_data = data;
	tx_off = 0;
	tx_len = len;
	atomic_set(&tx_active, 1);
	k_sem_reset(&tx_done);
	uart_irq_tx_enable(usb);

	deadline = k_uptime_get() + 120000;
	while (k_sem_take(&tx_done, K_MSEC(100)) != 0) {
		if (!host_is_open()) {
			stop_tx();
			return -ECONNABORTED;
		}
		if (k_uptime_get() >= deadline) {
			stop_tx();
			return -ETIMEDOUT;
		}
	}
	return 0;
}

static int send_frame(uint32_t count, int16_t status, const uint8_t *payload, size_t payload_len)
{
	uint8_t hdr[8];
	int err;

	hdr[0] = 0x5a;
	hdr[1] = 0xa5;
	hdr[2] = count;
	hdr[3] = count >> 8;
	hdr[4] = count >> 16;
	hdr[5] = count >> 24;
	hdr[6] = (uint16_t)status;
	hdr[7] = (uint16_t)status >> 8;

	err = send_bytes(hdr, sizeof(hdr));
	if (err || payload == NULL || payload_len == 0) {
		return err;
	}
	return send_bytes(payload, payload_len);
}

static void put_i32(uint8_t *dst, int32_t value)
{
	dst[0] = value;
	dst[1] = value >> 8;
	dst[2] = value >> 16;
	dst[3] = value >> 24;
}

static int sdram_test(void)
{
	volatile uint32_t *mem = (volatile uint32_t *)SDRAM_ADDR;
	uint32_t words = SDRAM_BYTES / sizeof(uint32_t);
	uint32_t last = words - 1U;

	for (uint32_t i = 0; i < words; i += 1024U) {
		mem[i] = 0x40800000U ^ i;
	}
	mem[last] = 0x5a5a5a5aU;

	for (uint32_t i = 0; i < words; i += 1024U) {
		if (mem[i] != (0x40800000U ^ i)) {
			return -EIO;
		}
	}
	if (mem[last] != 0x5a5a5a5aU) {
		return -EIO;
	}
	return 0;
}

static int capture_samples(uint32_t count)
{
	uint8_t *dst = (uint8_t *)SDRAM_ADDR;

	for (uint32_t done = 0; done < count;) {
		uint32_t n = count - done;
		int err;

		if (!host_is_open()) {
			return -ECONNABORTED;
		}
		if (n > FIFO_SAMPLES) {
			n = FIFO_SAMPLES;
		}

		options.interval_us = 0;
		options.extra_samplings = n - 1U;
		sequence.buffer_size = n * sizeof(samples[0]);

		err = adc_read_dt(&adc_channel, &sequence);
		if (err) {
			return err;
		}

		for (uint32_t i = 0; i < n; i++) {
			int32_t code = samples[i];

			*dst++ = code;
			*dst++ = code >> 8;
			*dst++ = code >> 16;
		}
		done += n;
	}

	return 0;
}

static int take_request(uint32_t *count)
{
	static uint8_t state;
	static uint32_t value;
	uint8_t byte;

	while (rx_get(&byte) == 0) {
		switch (state) {
		case 0:
			state = byte == 0xa5 ? 1 : 0;
			break;
		case 1:
			if (byte == 0x5a) {
				value = 0;
				state = 2;
			} else {
				state = byte == 0xa5 ? 1 : 0;
			}
			break;
		case 2:
		case 3:
		case 4:
			value |= (uint32_t)byte << ((state - 2) * 8);
			state++;
			break;
		case 5:
			*count = value | ((uint32_t)byte << 24);
			state = 0;
			return 1;
		default:
			state = 0;
			break;
		}
	}

	return 0;
}

static void handle_request(uint32_t count)
{
	uint8_t info[12];
	int err;

	if (count == 0) {
		put_i32(&info[0], FIFO_SAMPLES);
		put_i32(&info[4], RAM_SAMPLES);
		put_i32(&info[8], SAMPLE_HZ);
		send_frame(3, 0, info, sizeof(info));
		return;
	}

	if (count > RAM_SAMPLES) {
		send_frame(0, -EINVAL, NULL, 0);
		return;
	}

	err = capture_samples(count);
	if (err == -ECONNABORTED) {
		return;
	}
	if (err) {
		send_frame(0, (int16_t)err, NULL, 0);
		return;
	}

	send_frame(count, 0, (const uint8_t *)SDRAM_ADDR, count * 3U);
}

static int wait_for_host(void)
{
	uint32_t dtr = 0;

	printk("AD4080 USB high-speed waiting for the host\n");
	while (dtr == 0) {
		if (uart_line_ctrl_get(usb, UART_LINE_CTRL_DTR, &dtr) != 0) {
			return -ENOTSUP;
		}
		k_msleep(50);
	}
	return 0;
}

int main(void)
{
	const struct device *sdram = DEVICE_DT_GET(DT_NODELABEL(sdram));
	uint32_t request;
	int err;

	if (!device_is_ready(sdram)) {
		printk("SDRAM is not ready\n");
		return 0;
	}

	err = sdram_test();
	if (err) {
		printk("SDRAM test failed: %d\n", err);
		return 0;
	}

	if (!adc_is_ready_dt(&adc_channel)) {
		printk("AD4080 ADC device is not ready\n");
		return 0;
	}

	err = adc_channel_setup_dt(&adc_channel);
	if (err) {
		printk("ADC channel setup failed: %d\n", err);
		return 0;
	}

	err = adc_sequence_init_dt(&adc_channel, &sequence);
	if (err) {
		printk("ADC sequence init failed: %d\n", err);
		return 0;
	}

	usb = usb_serial_start();
	if (usb == NULL) {
		printk("USB device start failed\n");
		return 0;
	}

	uart_irq_callback_set(usb, usb_uart_cb);
	uart_irq_rx_enable(usb);

	printk("AD4080 ready, FIFO %u, SDRAM %u samples at %u Hz\n", FIFO_SAMPLES, RAM_SAMPLES,
	       SAMPLE_HZ);

	err = wait_for_host();
	if (err) {
		printk("USB host detect failed: %d\n", err);
		return 0;
	}

	while (1) {
		if (take_request(&request)) {
			handle_request(request);
		} else {
			k_yield();
		}
	}

	return 0;
}
