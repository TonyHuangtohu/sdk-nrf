/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */
#include <zephyr/types.h>
#include <stddef.h>
#include <inttypes.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <stdio.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/sys/printk.h>


#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/device.h>
#include <string.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>

static struct nvs_fs fs;

#define NVS_PARTITION		storage_partition
#define NVS_PARTITION_DEVICE	FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET	FIXED_PARTITION_OFFSET(NVS_PARTITION)

#define LOG_MODULE_NAME uart_thread
LOG_MODULE_REGISTER(LOG_MODULE_NAME,3);

#define UART_DEVICE_NAME         DT_NODE_FULL_NAME(DT_NODELABEL(my_uart))

#define UART_BUF_SIZE 255
#define UART_WAIT_FOR_BUF_DELAY K_MSEC(100)
#define UART_WAIT_FOR_RX 20000

static const struct device *uart;
typedef void (*rx_callback_t)(uint8_t *data, uint16_t len);
static rx_callback_t rx_callback;
const uint8_t help_string[] = "Send data from PC first, the connected device would echo back\r";

struct uart_data_t {
	void *fifo_reserved;
	uint8_t data[UART_BUF_SIZE];
	uint16_t len;
};

static uint8_t uart_rx_buf[2][UART_BUF_SIZE];
static uint8_t *next_buf = uart_rx_buf[1];

static K_FIFO_DEFINE(fifo_uart_tx_data);
static K_FIFO_DEFINE(fifo_uart_rx_data);


static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(dev);
	int err;

	switch (evt->type) {
	case UART_TX_DONE:
	{
		struct uart_data_t *buf;
		struct uart_data_t *buf2;
		
		buf = CONTAINER_OF(evt->data.tx.buf, struct uart_data_t,  data[0]);
		LOG_INF("UART_TX_DONE %d", evt->data.tx.len);
		k_free(buf);

		buf2 = k_fifo_get(&fifo_uart_tx_data, K_NO_WAIT);
		if (!buf2) {
			return;
		}

		if (uart_tx(uart, buf2->data, buf2->len, SYS_FOREVER_MS)) {
			LOG_WRN("uart_tx fail @ cb");
		}
	}
		break;

	case UART_RX_RDY:
	{
		struct uart_data_t *buf = k_malloc(sizeof(struct uart_data_t));
		memcpy(buf->data, &evt->data.rx.buf[evt->data.rx.offset], evt->data.rx.len);	
		buf->len = evt->data.rx.len;		
		k_fifo_put(&fifo_uart_rx_data, buf);
		LOG_INF("UART_RX_RDY %d", buf->len);
	}
		break;

	case UART_RX_DISABLED:
		LOG_INF("UART_RX_DISABLED");
		err = uart_rx_enable(uart, uart_rx_buf[0], sizeof(uart_rx_buf[0]), UART_WAIT_FOR_RX);
		if (err) {
			LOG_ERR("UART RX enable failed: %d", err);			
		}
		break;

	case UART_RX_BUF_REQUEST:
		err = uart_rx_buf_rsp(uart, next_buf,
			sizeof(uart_rx_buf[0]));
		if (err) {
			LOG_WRN("UART RX buf rsp: %d", err);
		}		
		break;

	case UART_RX_BUF_RELEASED:
		LOG_INF("UART_RX_BUF_RELEASED");
		next_buf = evt->data.rx_buf.buf;
		break;

	case UART_TX_ABORTED:
		LOG_INF("UART_TX_ABORTED");
		break;

	default:
		break;
	}
}

int uart_init(rx_callback_t cb)
{
	int err;
	
	uart = device_get_binding(UART_DEVICE_NAME);
	if (!uart) {
		return -ENXIO;
	}

	err = uart_callback_set(uart, uart_cb, NULL);
	if (err) {
		return err;
	}

	rx_callback = cb;

	return uart_rx_enable(uart, uart_rx_buf[0], sizeof(uart_rx_buf[0]), UART_WAIT_FOR_RX);

}

int uart_send(const uint8_t *buf, uint16_t len)
{
	int err;
	struct uart_data_t *tx = k_malloc(sizeof(*tx));

	if (!tx) {
		LOG_WRN("Not able to allocate UART send data buffer");
		return -ENOMEM; 
	}

	memcpy(tx->data, buf, len);	
	tx->len = len;
	err = uart_tx(uart, tx->data, tx->len, SYS_FOREVER_MS);
	if (err) {
		LOG_WRN("buffer uart tx data for later retry");
		k_fifo_put(&fifo_uart_tx_data, tx);
	}
	return err;		
}

void uart_receive()
{
	/* Wait indefinitely*/
	struct uart_data_t *buf = k_fifo_get(&fifo_uart_rx_data,
							K_FOREVER);
	rx_callback(buf->data,buf->len);							
	k_free(buf);		
}

void uart_rx_cb(uint8_t *data, uint16_t len)
{
	//int err;
	//err = uart_send(data, len);	
	//if (err)
	{
		//LOG_INF("uart send err %d", err);
	}
}

void uart_thread(void)
{
	int rc = 0;
	struct flash_pages_info info;

	/* define the nvs file system by settings with:
	 *	sector_size equal to the pagesize,
	 *	3 sectors
	 *	starting at NVS_PARTITION_OFFSET
	 */
	fs.flash_device = NVS_PARTITION_DEVICE;
	if (!device_is_ready(fs.flash_device)) {
		printk("Flash device %s is not ready\n", fs.flash_device->name);
		return ;
	}
	fs.offset = 0x7fa000;//NVS_PARTITION_OFFSET;
	rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
	if (rc) {
		printk("Unable to get page info\n");
		return ;
	}
	fs.sector_size = info.size;
	fs.sector_count = 3U;

	rc = nvs_mount(&fs);
	if (rc) {
		printk("Flash Init failed\n");
		return ;
	}

	
	int err = 0;

	LOG_INF("### high speed UART example %s %s\n", __TIME__, __DATE__);	

	err = uart_init(uart_rx_cb);
	if (err) {
		LOG_ERR("uart init err %d", err);
		return;
	}

	NRF_P0->PIN_CNF[30] = 0x3;
    NRF_GPIOTE->CONFIG[1] = 0x00131E03;
    //ppi channel configuration
    NRF_PPI->CH[9].EEP = (uint32_t)&NRF_UARTE0->EVENTS_RXDRDY;
    NRF_PPI->CH[9].TEP = (uint32_t)&NRF_GPIOTE->TASKS_OUT[1];
    NRF_PPI->CHENSET = (1<<9);
	//uart_send(help_string, sizeof(help_string));
	for (;;) {
		/* Wait indefinitely for UART data*/
		uart_receive();
	}
}

K_THREAD_DEFINE(uart_thread_id, 1024, uart_thread, NULL, NULL, NULL, 5, 0, 1000);