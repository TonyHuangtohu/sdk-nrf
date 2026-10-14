/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <bluetooth/services/nus.h>

#include <nrfx.h>

LOG_MODULE_REGISTER(peripheral_adc_continuous, LOG_LEVEL_INF);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define ADC_CHANNEL_COUNT 4
#define ADC_RESOLUTION 14
#define ADC_QUEUE_LEN (256*8)

/*
 * Set to 0 to use the legacy mode that calls adc_read_dt() once per sampled
 * channel. The default scan mode enables all four channels in one sequence.
 */
#ifndef ADC_USE_SCAN_MODE
#define ADC_USE_SCAN_MODE 1
#endif

/*
 * Scan mode only: selects how read_adc_scan() talks to the SAADC.
 * 1: program the SAADC registers directly and poll for completion (fast).
 * 0: use the Zephyr ADC driver, adc_read().
 */
#ifndef ADC_SCAN_USE_REGISTERS
#define ADC_SCAN_USE_REGISTERS 1
#endif

#define ADC_500HZ_DIVIDER 2
/*
 * 1 ms is not a whole number of system ticks (31250 Hz GRTC tick on nRF54L15),
 * so a periodic K_MSEC(1) timer rounds up to 32 ticks (1.024 ms, ~977 Hz).
 * The ADC thread therefore sleeps until absolute microsecond deadlines, which
 * keeps the average rate at exactly 1 kHz.
 */
#define ADC_PERIOD_US 1000
#define BLE_TX_BUF_LEN CONFIG_BT_L2CAP_TX_MTU
#define NUS_TX_IDLE_WAIT_INTERVAL K_MSEC(100)
#define NUS_TX_FLUSH_TIMEOUT_MS 50

#define BLE_STACK_SIZE 2048
#define BLE_PRIORITY 7
#define ADC_STACK_SIZE 2048
#define ADC_PRIORITY 5

/*
 * NUS TX logging: 3 kHz aggregate sample rate (500+500+1000+1000 Hz) would
 * flood RTT at LOG_INF.
 * LOG_DBG logs every sample on successful send; LOG_INF is rate-limited per
 * channel (every NUS_LOG_INF_INTERVAL samples). Raise CONFIG_LOG_DEFAULT_LEVEL
 * to 4 (DEBUG) to see per-sample output.
 *
 * ADC drop logging uses the same per-channel interval: LOG_WRN on the first
 * drop for a channel, then LOG_INF every ADC_DROP_LOG_INF_INTERVAL drops.
 */
#define NUS_LOG_INF_INTERVAL 100
#define ADC_DROP_LOG_INF_INTERVAL NUS_LOG_INF_INTERVAL
#define ADC_DROP_LED_PULSE_INTERVAL K_MSEC(100)

#define ADV_LED_BLINK_INTERVAL K_MSEC(500)
#define ADV_LED_NODE DT_ALIAS(led0)
#define CONN_LED_NODE DT_ALIAS(led1)
#define ADC_DROP_LED_NODE DT_ALIAS(led3)

/* BLE connection interval unit is 1.25 ms: 120 units = 150 ms. */
//#define CONN_INTERVAL_UNITS 144
#define CONN_INTERVAL_UNITS 120
#define CONN_SUPERVISION_TIMEOUT_UNITS 400

static const struct bt_le_conn_param pref_conn_param =
	BT_LE_CONN_PARAM_INIT(CONN_INTERVAL_UNITS, CONN_INTERVAL_UNITS, 0,
			      CONN_SUPERVISION_TIMEOUT_UNITS);

#if !DT_NODE_HAS_STATUS(ADV_LED_NODE, okay)
#error "Unsupported board: led0 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS(CONN_LED_NODE, okay)
#error "Unsupported board: led1 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS(ADC_DROP_LED_NODE, okay)
#error "Unsupported board: led3 devicetree alias is not defined"
#endif

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || \
	!DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No ADC channels configured. Add zephyr,user io-channels in the board overlay."
#endif

#define ADC_SPEC_GET_BY_IDX(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, ADC_SPEC_GET_BY_IDX)
};

BUILD_ASSERT(ARRAY_SIZE(adc_channels) == ADC_CHANNEL_COUNT,
	     "This sample expects exactly four ADC channels");

struct adc_sample {
	uint8_t source;
	int16_t raw;
	uint32_t timestamp_ms;
	uint32_t sequence;
};

struct adc_nus_frame {
	uint8_t sample_le[2];
} __packed;

#define ADC_PACKED_SOURCE_SHIFT ADC_RESOLUTION
#define ADC_PACKED_RAW_MASK ((1U << ADC_RESOLUTION) - 1U)

K_MSGQ_DEFINE(adc_queue_0, sizeof(struct adc_sample), ADC_QUEUE_LEN, 4);
K_MSGQ_DEFINE(adc_queue_1, sizeof(struct adc_sample), ADC_QUEUE_LEN, 4);
K_MSGQ_DEFINE(adc_queue_2, sizeof(struct adc_sample), ADC_QUEUE_LEN, 4);
K_MSGQ_DEFINE(adc_queue_3, sizeof(struct adc_sample), ADC_QUEUE_LEN, 4);

static struct k_msgq *const adc_queues[ADC_CHANNEL_COUNT] = {
	&adc_queue_0,
	&adc_queue_1,
	&adc_queue_2,
	&adc_queue_3,
};

enum adc_drop_reason {
	ADC_DROP_QUEUE_FULL,
	ADC_DROP_TX_BATCH_FULL,
	ADC_DROP_NUS_SEND_FAILED,
};

static uint32_t adc_sequences[ADC_CHANNEL_COUNT];
static uint32_t nus_tx_log_counts[ADC_CHANNEL_COUNT];
static uint32_t adc_drop_counts[ADC_CHANNEL_COUNT];

static const char *const adc_source_names[ADC_CHANNEL_COUNT] = {
	"AIN0", "AIN1", "AIN2", "AIN3",
};

static K_SEM_DEFINE(adc_ready, 0, 1);
/*
 * Wake-up signal only: the BLE thread drains every queue once it runs, so the
 * count must not track individual samples. An unbounded count grows by 3000
 * per second while nobody is subscribed, and draining it later stalls the
 * Bluetooth RX thread for a time proportional to how long the device was idle.
 */
static K_SEM_DEFINE(samples_ready, 0, 1);
static K_SEM_DEFINE(nus_ready, 0, 1);
static K_MUTEX_DEFINE(tx_cache_lock);

static const struct gpio_dt_spec adv_led = GPIO_DT_SPEC_GET(ADV_LED_NODE, gpios);
static const struct gpio_dt_spec conn_led = GPIO_DT_SPEC_GET(CONN_LED_NODE, gpios);
static const struct gpio_dt_spec adc_drop_led = GPIO_DT_SPEC_GET(ADC_DROP_LED_NODE, gpios);

static struct bt_conn *current_conn;
static struct k_work adv_work;
static struct k_mutex conn_lock;
static atomic_t ble_connected;
static atomic_t advertising_led_enabled;
static atomic_t nus_notify_enabled;
static bool advertising_led_on;
static uint8_t pending_tx_buf[BLE_TX_BUF_LEN];
static size_t pending_tx_len;
static int64_t pending_tx_started_at_ms;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static void advertising_led_work_handler(struct k_work *work);
static void adc_drop_led_work_handler(struct k_work *work);
static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx);

static K_WORK_DELAYABLE_DEFINE(advertising_led_work, advertising_led_work_handler);
static K_WORK_DELAYABLE_DEFINE(adc_drop_led_work, adc_drop_led_work_handler);
static struct bt_gatt_cb gatt_callbacks = {
	.att_mtu_updated = att_mtu_updated,
};

static void reset_pending_tx_cache(void)
{
	k_mutex_lock(&tx_cache_lock, K_FOREVER);
	pending_tx_len = 0;
	pending_tx_started_at_ms = 0;
	k_mutex_unlock(&tx_cache_lock);
}

static void clear_pending_samples(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(adc_queues); i++) {
		k_msgq_purge(adc_queues[i]);
	}

	reset_pending_tx_cache();

	k_sem_reset(&samples_ready);
}

static int leds_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&adv_led)) {
		LOG_ERR("Advertising LED GPIO device is not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&conn_led)) {
		LOG_ERR("Connection LED GPIO device is not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&adc_drop_led)) {
		LOG_ERR("ADC drop LED GPIO device is not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&adv_led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Advertising LED configure failed: %d", err);
		return err;
	}

	err = gpio_pin_configure_dt(&conn_led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Connection LED configure failed: %d", err);
		return err;
	}

	err = gpio_pin_configure_dt(&adc_drop_led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("ADC drop LED configure failed: %d", err);
		return err;
	}

	return 0;
}

static void led_set_connected(bool connected)
{
	(void)gpio_pin_set_dt(&conn_led, connected);
}

static void led_set_advertising(bool enabled)
{
	if (enabled) {
		advertising_led_on = false;
		(void)gpio_pin_set_dt(&adv_led, 0);
		atomic_set(&advertising_led_enabled, true);
		(void)k_work_schedule(&advertising_led_work, K_NO_WAIT);
		return;
	}

	atomic_clear(&advertising_led_enabled);
	(void)k_work_cancel_delayable(&advertising_led_work);
	advertising_led_on = false;
	(void)gpio_pin_set_dt(&adv_led, 0);
}

static void advertising_led_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!atomic_get(&advertising_led_enabled)) {
		advertising_led_on = false;
		(void)gpio_pin_set_dt(&adv_led, 0);
		return;
	}

	advertising_led_on = !advertising_led_on;
	(void)gpio_pin_set_dt(&adv_led, advertising_led_on);
	(void)k_work_schedule(&advertising_led_work, ADV_LED_BLINK_INTERVAL);
}

static void adc_drop_led_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (atomic_get(&advertising_led_enabled)) {
		return;
	}

	(void)gpio_pin_set_dt(&adc_drop_led, 0);
}

static void led_indicate_adc_drop(void)
{
	if (atomic_get(&advertising_led_enabled)) {
		return;
	}

	(void)gpio_pin_set_dt(&adc_drop_led, 1);
	(void)k_work_reschedule(&adc_drop_led_work, ADC_DROP_LED_PULSE_INTERVAL);
}

static const char *adc_drop_reason_str(enum adc_drop_reason reason)
{
	switch (reason) {
	case ADC_DROP_QUEUE_FULL:
		return "queue full";
	case ADC_DROP_TX_BATCH_FULL:
		return "TX batch full";
	case ADC_DROP_NUS_SEND_FAILED:
		return "NUS send failed";
	default:
		return "unknown";
	}
}

static void log_adc_sample_dropped(uint8_t source, enum adc_drop_reason reason,
				   const struct adc_sample *sample, struct k_msgq *queue)
{
	size_t queue_used;

	if (source >= ADC_CHANNEL_COUNT) {
		return;
	}

	queue_used = queue ? k_msgq_num_used_get(queue) : 0U;
	adc_drop_counts[source]++;

	if (reason == ADC_DROP_QUEUE_FULL) {
		led_indicate_adc_drop();
	}

	LOG_DBG("ADC drop ch=%s (%u) reason=%s seq=%u queue_used=%u/%u",
		adc_source_names[source], source, adc_drop_reason_str(reason),
		sample ? sample->sequence : 0U, queue_used, ADC_QUEUE_LEN);

	if (adc_drop_counts[source] == 1U) {
		LOG_WRN("ADC drop ch=%s (%u) reason=%s seq=%u queue_used=%u/%u (first)",
			adc_source_names[source], source, adc_drop_reason_str(reason),
			sample ? sample->sequence : 0U, queue_used, ADC_QUEUE_LEN);
	} else if ((adc_drop_counts[source] % ADC_DROP_LOG_INF_INTERVAL) == 0U) {
		LOG_INF("ADC drop ch=%s (%u) reason=%s total_drops=%u queue_used=%u/%u",
			adc_source_names[source], source, adc_drop_reason_str(reason),
			adc_drop_counts[source], queue_used, ADC_QUEUE_LEN);
	}
}

static void log_adc_batch_dropped(const uint8_t *tx_buf, size_t len,
				  enum adc_drop_reason reason)
{
	size_t offset = 0;

	while (offset + sizeof(struct adc_nus_frame) <= len) {
		const struct adc_nus_frame *frame =
			(const struct adc_nus_frame *)&tx_buf[offset];
		uint16_t packed = sys_get_le16(frame->sample_le);
		uint8_t source = packed >> ADC_PACKED_SOURCE_SHIFT;
		struct adc_sample sample = {
			.source = source,
			.raw = (int16_t)(packed & ADC_PACKED_RAW_MASK),
		};

		if (source < ADC_CHANNEL_COUNT) {
			log_adc_sample_dropped(source, reason, &sample, adc_queues[source]);
		}
		offset += sizeof(struct adc_nus_frame);
	}
}

static void store_adc_sample(uint8_t source, int16_t raw, uint32_t timestamp_ms)
{
	struct adc_sample sample = {
		.source = source,
		.raw = raw,
		.timestamp_ms = timestamp_ms,
		.sequence = adc_sequences[source]++,
	};
	struct adc_sample dropped;
	struct k_msgq *queue = adc_queues[source];

	while (k_msgq_put(queue, &sample, K_NO_WAIT) != 0) {
		/* Keep the newest continuous samples if BLE cannot keep up. */
		if (k_msgq_get(queue, &dropped, K_NO_WAIT) == 0) {
			log_adc_sample_dropped(source, ADC_DROP_QUEUE_FULL, &dropped, queue);
		}
	}

	k_sem_give(&samples_ready);
}

#if ADC_USE_SCAN_MODE
#if ADC_SCAN_USE_REGISTERS
/*
 * Direct register access to the SAADC. The Zephyr ADC driver still owns the
 * peripheral at boot (adc_channel_setup_dt() validates the devicetree), but
 * read_adc_scan() never calls adc_read() in this mode, so the driver does not
 * touch the SAADC afterwards.
 *
 * The values below are the ones the Zephyr driver programs for the board
 * overlay (read back from hardware), so both modes produce identical samples.
 * Note that nRF54L15 encodes these registers differently from nRF52.
 */

/* nRF54L15 SAADC analog inputs AIN0..AIN3 are P1.04..P1.07. */
#define SAADC_REG_AIN_PORT 1
static const uint8_t saadc_reg_ain_pin[ADC_CHANNEL_COUNT] = { 4, 5, 6, 7 };

/* Acquisition time is (TACQ + 1) x 125 ns: 79 gives the 10 us driver default. */
#define SAADC_REG_TACQ_10US 79

/* Gain 1/4 (GAIN = 2/8), internal 0.9 V reference, single ended, no burst. */
#define SAADC_REG_CH_CONFIG                                                    \
	((SAADC_CH_CONFIG_GAIN_Gain2_8 << SAADC_CH_CONFIG_GAIN_Pos) |          \
	 (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |     \
	 (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) |               \
	 (SAADC_REG_TACQ_10US << SAADC_CH_CONFIG_TACQ_Pos))

/*
 * Upper bound for each busy-wait. One 4-channel scan takes about 50 us; this
 * many polls is well over 1 ms at 128 MHz and only guards against a stuck ADC.
 */
#define SAADC_REG_POLL_LIMIT 200000U

BUILD_ASSERT(ADC_RESOLUTION == 14, "SAADC register setup assumes 14-bit resolution");
BUILD_ASSERT(ADC_CHANNEL_COUNT <= ARRAY_SIZE(NRF_SAADC->CH));

static int adc_regs_check_dt(void)
{
	/* Keep the hardcoded register values in sync with the board overlay. */
	for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++) {
		const struct adc_channel_cfg *cfg = &adc_channels[i].channel_cfg;

		if (!adc_channels[i].channel_cfg_dt_node_exists ||
		    cfg->input_positive != (NRF_SAADC_AIN0 + i) ||
		    cfg->gain != ADC_GAIN_1_4 || cfg->reference != ADC_REF_INTERNAL ||
		    cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT || cfg->differential) {
			LOG_ERR("ADC channel %u devicetree config does not match the "
				"SAADC register setup", i);
			return -EINVAL;
		}
	}

	return 0;
}

static int adc_regs_init(void)
{
	int err = adc_regs_check_dt();

	if (err) {
		return err;
	}

	/* Completion is polled: keep the Zephyr driver ISR from consuming events. */
	NRF_SAADC->INTENCLR = UINT32_MAX;
	NRF_SAADC->SHORTS = 0;

	for (size_t i = 0; i < ARRAY_SIZE(NRF_SAADC->CH); i++) {
		if (i < ADC_CHANNEL_COUNT) {
			NRF_SAADC->CH[i].PSELP =
				(saadc_reg_ain_pin[i] << SAADC_CH_PSELP_PIN_Pos) |
				(SAADC_REG_AIN_PORT << SAADC_CH_PSELP_PORT_Pos) |
				(SAADC_CH_PSELP_CONNECT_AnalogInput << SAADC_CH_PSELP_CONNECT_Pos);
			NRF_SAADC->CH[i].CONFIG = SAADC_REG_CH_CONFIG;
		} else {
			NRF_SAADC->CH[i].PSELP =
				SAADC_CH_PSELP_CONNECT_NC << SAADC_CH_PSELP_CONNECT_Pos;
		}
		NRF_SAADC->CH[i].PSELN = SAADC_CH_PSELN_CONNECT_NC << SAADC_CH_PSELN_CONNECT_Pos;
	}

	NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_14bit << SAADC_RESOLUTION_VAL_Pos;
	NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
	NRF_SAADC->SAMPLERATE = SAADC_SAMPLERATE_MODE_Task << SAADC_SAMPLERATE_MODE_Pos;

	/* Enabled only for the duration of each scan, like the Zephyr driver. */
	NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos;

	return 0;
}

static void saadc_reg_disable(void)
{
	/*
	 * Same sequence as the nrfx driver (saadc_disable()): ENABLE=0 does not
	 * fully power down the SAADC core unless STOP is triggered first.
	 */
	NRF_SAADC->TASKS_STOP = 1;
	NRF_SAADC->EVENTS_STOPPED = 0;
	/* The driver writes ENABLE=0 twice; kept for parity. */
	NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos;
	NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos;
}

static bool saadc_reg_wait(volatile uint32_t *event)
{
	for (uint32_t i = 0; i < SAADC_REG_POLL_LIMIT; i++) {
		if (*event) {
			*event = 0;
			return true;
		}
	}

	return false;
}

static int read_adc_scan(int16_t raw[ADC_CHANNEL_COUNT])
{
	NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos;

	/* MAXCNT is in bytes on nRF54L15. */
	NRF_SAADC->RESULT.PTR = (uint32_t)raw;
	NRF_SAADC->RESULT.MAXCNT = sizeof(raw[0]) * ADC_CHANNEL_COUNT;
	NRF_SAADC->EVENTS_STARTED = 0;
	NRF_SAADC->EVENTS_END = 0;

	NRF_SAADC->TASKS_START = 1;
	if (!saadc_reg_wait(&NRF_SAADC->EVENTS_STARTED)) {
		goto timeout;
	}

	/* One SAMPLE task converts every enabled channel and fills the buffer. */
	NRF_SAADC->TASKS_SAMPLE = 1;
	if (!saadc_reg_wait(&NRF_SAADC->EVENTS_END)) {
		goto timeout;
	}

	saadc_reg_disable();

	/* EasyDMA wrote raw[] behind the compiler's back. */
	barrier_dmem_fence_full();

	/*
	 * Single-ended results can be slightly negative near 0 V because of the
	 * ADC offset. Clamp them to 0 like the Zephyr driver does
	 * (correct_single_ended()); the 14-bit NUS record cannot carry a sign.
	 */
	for (size_t i = 0; i < ADC_CHANNEL_COUNT; i++) {
		if (raw[i] < 0) {
			raw[i] = 0;
		}
	}

	return 0;

timeout:
	NRF_SAADC->EVENTS_STOPPED = 0;
	NRF_SAADC->TASKS_STOP = 1;
	(void)saadc_reg_wait(&NRF_SAADC->EVENTS_STOPPED);
	saadc_reg_disable();
	return -ETIMEDOUT;
}
#else
static uint32_t adc_scan_channel_mask(void)
{
	uint32_t mask = 0;

	for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++) {
		mask |= BIT(adc_channels[i].channel_id);
	}

	return mask;
}

static int read_adc_scan(int16_t raw[ADC_CHANNEL_COUNT])
{
	struct adc_sequence sequence = {
		.channels = adc_scan_channel_mask(),
		.buffer = raw,
		.buffer_size = sizeof(raw[0]) * ADC_CHANNEL_COUNT,
		.resolution = ADC_RESOLUTION,
	};

	return adc_read(adc_channels[0].dev, &sequence);
}
#endif /* ADC_SCAN_USE_REGISTERS */

static void sample_scan(uint32_t tick)
{
	int16_t raw[ADC_CHANNEL_COUNT];
	uint32_t timestamp_ms;
	int err = read_adc_scan(raw);

	if (err) {
		LOG_WRN("ADC scan read failed: %d", err);
		return;
	}

	timestamp_ms = (uint32_t)k_uptime_get();

	for (uint8_t source = 0; source < ADC_CHANNEL_COUNT; source++) {
		if ((source < 2U) && ((tick % ADC_500HZ_DIVIDER) != 0U)) {
			continue;
		}

		store_adc_sample(source, raw[source], timestamp_ms);
	}
}
#else
static int read_adc_channel(uint8_t source, int16_t *raw)
{
	struct adc_sequence sequence = {
		.buffer = raw,
		.buffer_size = sizeof(*raw),
		.resolution = ADC_RESOLUTION,
	};

	(void)adc_sequence_init_dt(&adc_channels[source], &sequence);
	sequence.resolution = ADC_RESOLUTION;

	return adc_read_dt(&adc_channels[source], &sequence);
}

static void sample_channel(uint8_t source)
{
	int16_t raw;
	int err = read_adc_channel(source, &raw);

	if (err) {
		LOG_WRN("ADC channel %u read failed: %d", source, err);
		return;
	}

	store_adc_sample(source, raw, (uint32_t)k_uptime_get());
}
#endif /* ADC_USE_SCAN_MODE */

static int adc_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++) {
		int err;

		if (!adc_is_ready_dt(&adc_channels[i])) {
			LOG_ERR("ADC controller %s is not ready", adc_channels[i].dev->name);
			return -ENODEV;
		}

		err = adc_channel_setup_dt(&adc_channels[i]);
		if (err) {
			LOG_ERR("ADC channel %u setup failed: %d", i, err);
			return err;
		}

#if ADC_USE_SCAN_MODE
		if (adc_channels[i].dev != adc_channels[0].dev) {
			LOG_ERR("ADC scan requires all channels on the same controller");
			return -EINVAL;
		}

		if (adc_channels[i].channel_id != i) {
			LOG_ERR("ADC scan requires channel IDs 0..%u in buffer order",
				ADC_CHANNEL_COUNT - 1U);
			return -EINVAL;
		}
#endif /* ADC_USE_SCAN_MODE */
	}

#if ADC_USE_SCAN_MODE && ADC_SCAN_USE_REGISTERS
	return adc_regs_init();
#else
	return 0;
#endif
}

static void adc_thread(void)
{
	uint32_t tick = 0;
	int64_t next_us;

	k_sem_take(&adc_ready, K_FOREVER);
	next_us = k_ticks_to_us_ceil64(k_uptime_ticks());

	while (true) {
		next_us += ADC_PERIOD_US;
		k_sleep(K_TIMEOUT_ABS_US(next_us));

#if ADC_USE_SCAN_MODE
		sample_scan(tick);
#else
		if ((tick % ADC_500HZ_DIVIDER) == 0) {
			sample_channel(0);
			sample_channel(1);
		}

		sample_channel(2);
		sample_channel(3);
#endif /* ADC_USE_SCAN_MODE */

		tick++;
	}
}

static struct bt_conn *get_nus_conn(void)
{
	struct bt_conn *conn = NULL;

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (current_conn && atomic_get(&nus_notify_enabled)) {
		conn = bt_conn_ref(current_conn);
	}
	k_mutex_unlock(&conn_lock);

	return conn;
}

static void encode_frame(const struct adc_sample *sample, struct adc_nus_frame *frame)
{
	uint16_t packed = ((uint16_t)sample->source << ADC_PACKED_SOURCE_SHIFT) |
			  ((uint16_t)sample->raw & ADC_PACKED_RAW_MASK);

	sys_put_le16(packed, frame->sample_le);
}

static void log_nus_samples_sent(const uint8_t *tx_buf, size_t len)
{
	size_t offset = 0;

	while (offset + sizeof(struct adc_nus_frame) <= len) {
		const struct adc_nus_frame *frame =
			(const struct adc_nus_frame *)&tx_buf[offset];
		uint16_t packed = sys_get_le16(frame->sample_le);
		uint8_t source = packed >> ADC_PACKED_SOURCE_SHIFT;
		uint16_t raw = packed & ADC_PACKED_RAW_MASK;

		if (source < ADC_CHANNEL_COUNT) {
			LOG_DBG("NUS TX ch=%s (%u) raw=%u",
				adc_source_names[source], source, raw);

			nus_tx_log_counts[source]++;
			if ((nus_tx_log_counts[source] % NUS_LOG_INF_INTERVAL) == 0) {
				LOG_INF("NUS TX ch=%s (%u) raw=%u",
					adc_source_names[source], source, raw);
			}
		}

		offset += sizeof(struct adc_nus_frame);
	}
}

static size_t append_one_sample(uint8_t source, uint8_t *tx_buf, size_t len, size_t tx_limit)
{
	struct adc_sample sample;
	struct adc_nus_frame frame;

	if (k_msgq_get(adc_queues[source], &sample, K_NO_WAIT) != 0) {
		return len;
	}

	encode_frame(&sample, &frame);
	if ((len + sizeof(frame)) > tx_limit) {
		log_adc_sample_dropped(source, ADC_DROP_TX_BATCH_FULL, &sample,
				       adc_queues[source]);
		return len;
	}

	memcpy(&tx_buf[len], &frame, sizeof(frame));
	return len + sizeof(frame);
}

static bool fill_pending_tx_cache(uint8_t *next_source)
{
	bool appended = false;

	while ((pending_tx_len + sizeof(struct adc_nus_frame)) <= sizeof(pending_tx_buf)) {
		size_t previous_len = pending_tx_len;

		for (uint8_t i = 0; i < ADC_CHANNEL_COUNT; i++) {
			uint8_t source = (*next_source + i) % ADC_CHANNEL_COUNT;

			pending_tx_len = append_one_sample(source, pending_tx_buf, pending_tx_len,
						   sizeof(pending_tx_buf));
			if (pending_tx_len != previous_len) {
				if (previous_len == 0U) {
					pending_tx_started_at_ms = k_uptime_get();
				}

				*next_source = (source + 1) % ADC_CHANNEL_COUNT;
				appended = true;
				break;
			}
		}

		if (pending_tx_len == previous_len) {
			break;
		}
	}

	return appended;
}

static int64_t pending_tx_flush_remaining_ms(int64_t now_ms)
{
	int64_t elapsed_ms;

	if (pending_tx_len == 0U || pending_tx_started_at_ms == 0) {
		return -1;
	}

	elapsed_ms = now_ms - pending_tx_started_at_ms;
	if (elapsed_ms >= NUS_TX_FLUSH_TIMEOUT_MS) {
		return 0;
	}

	return NUS_TX_FLUSH_TIMEOUT_MS - elapsed_ms;
}

static k_timeout_t pending_tx_wait_timeout(void)
{
	int64_t remaining_ms;

	k_mutex_lock(&tx_cache_lock, K_FOREVER);
	remaining_ms = pending_tx_flush_remaining_ms(k_uptime_get());
	k_mutex_unlock(&tx_cache_lock);

	if (remaining_ms < 0) {
		return NUS_TX_IDLE_WAIT_INTERVAL;
	}

	if (remaining_ms == 0) {
		return K_NO_WAIT;
	}

	return K_MSEC(remaining_ms);
}

static bool send_available_samples(struct bt_conn *conn, uint8_t *next_source)
{
	uint8_t tx_buf[BLE_TX_BUF_LEN];
	size_t tx_limit = MIN(bt_nus_get_mtu(conn), sizeof(tx_buf));
	size_t send_limit = tx_limit - (tx_limit % sizeof(struct adc_nus_frame));
	size_t len;
	int64_t now_ms;
	int64_t remaining_ms;
	int err;

	if (send_limit == 0U) {
		return false;
	}

	k_mutex_lock(&tx_cache_lock, K_FOREVER);
	(void)fill_pending_tx_cache(next_source);

	if (pending_tx_len == 0U) {
		k_mutex_unlock(&tx_cache_lock);
		return false;
	}

	now_ms = k_uptime_get();
	remaining_ms = pending_tx_flush_remaining_ms(now_ms);
	if (pending_tx_len < send_limit && remaining_ms > 0) {
		k_mutex_unlock(&tx_cache_lock);
		return false;
	}

	len = MIN(pending_tx_len, send_limit);
	memcpy(tx_buf, pending_tx_buf, len);
	pending_tx_len -= len;
	if (pending_tx_len > 0U) {
		memmove(pending_tx_buf, &pending_tx_buf[len], pending_tx_len);
		pending_tx_started_at_ms = now_ms;
	} else {
		pending_tx_started_at_ms = 0;
	}
	k_mutex_unlock(&tx_cache_lock);

	if (len == 0) {
		return false;
	}

	err = bt_nus_send(conn, tx_buf, len);
	if (err) {
		log_adc_batch_dropped(tx_buf, len, ADC_DROP_NUS_SEND_FAILED);
		LOG_WRN("NUS send failed: %d (%u bytes, %u frames)", err, len,
			(uint32_t)(len / sizeof(struct adc_nus_frame)));
		k_sleep(K_MSEC(10));
	} else {
		log_nus_samples_sent(tx_buf, len);
		LOG_INF("NUS TX %u bytes (%u frames), tx_limit=%u", len, (uint32_t)(len / sizeof(struct adc_nus_frame)), tx_limit);
	}

	return true;
}

static const char *le_phy_to_str(uint8_t phy)
{
	switch (phy) {
	case BT_GAP_LE_PHY_1M:
		return "1M";
	case BT_GAP_LE_PHY_2M:
		return "2M";
	case BT_GAP_LE_PHY_CODED:
		return "Coded";
	default:
		return "unknown";
	}
}

static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("ATT MTU updated: %s TX=%u RX=%u", addr, tx, rx);
}

#if defined(CONFIG_BT_USER_PHY_UPDATE)
static void le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("LE PHY updated: %s TX %s (0x%x) RX %s (0x%x)", addr,
		le_phy_to_str(param->tx_phy), param->tx_phy,
		le_phy_to_str(param->rx_phy), param->rx_phy);
}
#endif /* CONFIG_BT_USER_PHY_UPDATE */

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	/* Integer formatting: CONFIG_CBPRINTF_FP_SUPPORT is not enabled. */
	LOG_INF("LE conn param updated: %s interval=%u (%u.%02u ms) latency=%u timeout=%u (%u ms)",
		addr, interval, (interval * 125U) / 100U, (interval * 125U) % 100U, latency,
		timeout, timeout * 10U);
}

static void request_link_tuning(struct bt_conn *conn)
{
	int err;

	err = bt_conn_le_param_update(conn, &pref_conn_param);
	if (err && err != -EALREADY) {
		LOG_WRN("Connection parameter update request failed: %d", err);
	}
}

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	ARG_UNUSED(work);

	if (err == -EALREADY) {
		led_set_advertising(true);
		return;
	}

	if (err) {
		led_set_advertising(false);
		LOG_ERR("Advertising start failed: %d", err);
		return;
	}

	led_set_advertising(true);
	LOG_INF("Advertising started");
}

static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

static void ble_send_thread(void)
{
	uint8_t next_source = 0;

	while (true) {
		bool sent = false;
		struct bt_conn *conn = get_nus_conn();

		if (!conn) {
			k_sem_take(&nus_ready, NUS_TX_IDLE_WAIT_INTERVAL);
			continue;
		}

		sent = send_available_samples(conn, &next_source);

		bt_conn_unref(conn);

		if (!sent) {
			k_sem_take(&samples_ready, pending_tx_wait_timeout());
		}
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("Connection failed: 0x%02x %s", err, bt_hci_err_to_str(err));
		atomic_clear(&ble_connected);
		led_set_connected(false);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected: %s", addr);

	k_mutex_lock(&conn_lock, K_FOREVER);
	current_conn = bt_conn_ref(conn);
	k_mutex_unlock(&conn_lock);

	atomic_set(&ble_connected, true);
	led_set_advertising(false);
	led_set_connected(true);

	request_link_tuning(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected: %s, reason 0x%02x %s", addr, reason,
		bt_hci_err_to_str(reason));

	atomic_clear(&nus_notify_enabled);
	atomic_clear(&ble_connected);

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	k_mutex_unlock(&conn_lock);

	reset_pending_tx_cache();
	led_set_connected(false);
	led_set_advertising(true);
}

static void recycled_cb(void)
{
	LOG_INF("Connection object recycled");
	advertising_start();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled_cb,
	.le_param_updated = le_param_updated,
#if defined(CONFIG_BT_USER_PHY_UPDATE)
	.le_phy_updated = le_phy_updated,
#endif
};

static void nus_send_enabled(enum bt_nus_send_status status)
{
	bool enabled = (status == BT_NUS_SEND_STATUS_ENABLED);

	atomic_set(&nus_notify_enabled, enabled);
	LOG_INF("NUS notifications %s", enabled ? "enabled" : "disabled");

	led_set_connected(atomic_get(&ble_connected));

	if (enabled) {
		LOG_INF("Clearing pending ADC samples on NUS notify enable");
		clear_pending_samples();
		k_sem_give(&nus_ready);
	}
}

static void nus_received(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(data);

	LOG_DBG("Received %u bytes over NUS", len);
}

static struct bt_nus_cb nus_cb = {
	.received = nus_received,
	.send_enabled = nus_send_enabled,
};

int main(void)
{
	int err;

	k_mutex_init(&conn_lock);

	err = leds_init();
	if (err) {
		return err;
	}

	err = adc_init();
	if (err) {
		return err;
	}
	k_sem_give(&adc_ready);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed: %d", err);
		return err;
	}

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("NUS init failed: %d", err);
		return err;
	}

	bt_gatt_cb_register(&gatt_callbacks);

	k_work_init(&adv_work, adv_work_handler);
	advertising_start();

	LOG_INF("ADC NUS sample started");
	return 0;
}

K_THREAD_DEFINE(adc_thread_id, ADC_STACK_SIZE, adc_thread, NULL, NULL, NULL,
		ADC_PRIORITY, 0, 0);

K_THREAD_DEFINE(ble_send_thread_id, BLE_STACK_SIZE, ble_send_thread, NULL, NULL, NULL,
		BLE_PRIORITY, 0, 0);
