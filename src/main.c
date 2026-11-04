/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read a short burst from the AD4080 through the Zephyr ADC API and print
 * the signed codes plus the differential voltage.
 */

#include <stdint.h>

#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define BURST_SAMPLES 32

static const struct adc_dt_spec adc_channel = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static int32_t samples[BURST_SAMPLES];

static void print_burst(void)
{
	int64_t sum = 0;

	printk("samples:");
	for (int i = 0; i < BURST_SAMPLES; i++) {
		int32_t millivolts = samples[i];

		sum += samples[i];
		if (i < 8) {
			adc_raw_to_millivolts_dt(&adc_channel, &millivolts);
			printk(" %d (%d mV)", samples[i], millivolts);
		}
	}

	int32_t average = (int32_t)(sum / BURST_SAMPLES);
	int32_t average_mv = average;

	adc_raw_to_millivolts_dt(&adc_channel, &average_mv);
	printk("\naverage of %d samples: %d (%d mV)\n", BURST_SAMPLES, average, average_mv);
}

int main(void)
{
	struct adc_sequence_options options = {
		.extra_samplings = BURST_SAMPLES - 1,
	};
	struct adc_sequence sequence = {
		.buffer = samples,
		.buffer_size = sizeof(samples),
		.options = &options,
	};
	int err;

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

	printk("AD4080 ready, capturing %d samples per burst\n", BURST_SAMPLES);

	while (1) {
		err = adc_read_dt(&adc_channel, &sequence);
		if (err) {
			printk("ADC read failed: %d\n", err);
		} else {
			print_burst();
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}
