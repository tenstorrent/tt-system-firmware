/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT tenstorrent_vuart

#include <errno.h>
#include <stdatomic.h>

#include <tenstorrent/uart_tt_virt.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(uart_tt_virt, CONFIG_UART_LOG_LEVEL);

struct uart_tt_virt_config {
	volatile struct tt_vuart *vuart;
	bool init_metadata;
	bool loopback;
	uint32_t magic;
	uint32_t version;
	uint32_t rx_cap;
	uint32_t tx_cap;
};

struct uart_tt_virt_data {
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	struct uart_config cfg;
#endif

	/* Descriptor in use, either the one from the config or one adopted from a previous stage */
	volatile struct tt_vuart *vuart;

	uint32_t err_flags;

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	struct k_spinlock vuart_lock;

	bool err_irq_en;
	bool rx_irq_en;
	bool tx_irq_en;
	struct k_timer irq_timer;
	const struct device *dev;

	uart_irq_callback_user_data_t irq_cb;
	void *irq_cb_udata;
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
};

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int uart_tt_virt_irq_is_pending(const struct device *dev);
static int uart_tt_virt_irq_rx_ready(const struct device *dev);
static int uart_tt_virt_irq_tx_ready(const struct device *dev);
#endif

#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
static int uart_tt_virt_config_get(const struct device *dev, struct uart_config *cfg)
{
	struct uart_tt_virt_data *data = dev->data;

	data->cfg = *cfg;
	return 0;
}

static int uart_tt_virt_configure(const struct device *dev, const struct uart_config *cfg)
{
	struct uart_tt_virt_data *data = dev->data;

	if (cfg == NULL) {
		return -EINVAL;
	}

	if (!((cfg->parity >= UART_CFG_PARITY_NONE) && (cfg->parity <= UART_CFG_PARITY_SPACE))) {
		return -EINVAL;
	}

	if (!((cfg->stop_bits >= UART_CFG_STOP_BITS_0_5) &&
	      (cfg->stop_bits <= UART_CFG_STOP_BITS_2))) {
		return -EINVAL;
	}

	if (!((cfg->data_bits >= UART_CFG_DATA_BITS_5) &&
	      (cfg->data_bits <= UART_CFG_DATA_BITS_8))) {
		return -EINVAL;
	}

	if (!((cfg->flow_ctrl >= UART_CFG_FLOW_CTRL_NONE) &&
	      (cfg->flow_ctrl <= UART_CFG_FLOW_CTRL_RTS_CTS))) {
		return -EINVAL;
	}

	data->cfg = *cfg;
	return 0;
}
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */

static int uart_tt_virt_err_check(const struct device *dev)
{
	struct uart_tt_virt_data *data = dev->data;

	return !!data->err_flags;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int uart_tt_virt_fifo_fill(const struct device *dev, const uint8_t *tx_data, int size)
{
	struct uart_tt_virt_data *data = dev->data;
	const struct uart_tt_virt_config *config = dev->config;
	volatile struct tt_vuart *vuart = data->vuart;

	__ASSERT_NO_MSG(size >= 0);

	K_SPINLOCK(&data->vuart_lock) {
		size = MIN((int)tt_vuart_buf_space(vuart->tx_head, vuart->tx_tail, vuart->tx_cap),
			   size);

		for (int i = 0; i < size; ++i) {
			tt_vuart_poll_out(vuart, *tx_data++, TT_VUART_ROLE_DEVICE);
		}
	}

	if (config->loopback && size > 0) {
		K_SPINLOCK(&data->vuart_lock) {
			int lim = MIN(size, (int)tt_vuart_buf_space(vuart->rx_head, vuart->rx_tail,
								    vuart->rx_cap));

			for (int i = 0; i < lim; ++i) {
				unsigned char ch = -1;

				(void)tt_vuart_poll_in(vuart, &ch, TT_VUART_ROLE_HOST);
				tt_vuart_poll_out(vuart, ch, TT_VUART_ROLE_HOST);
			}

			/* Note: irq_handler() picks up rx data */
		}
	}

	return size;
}

static int uart_tt_virt_fifo_read(const struct device *dev, uint8_t *rx_data, int size)
{
	struct uart_tt_virt_data *data = dev->data;
	volatile struct tt_vuart *vuart = data->vuart;

	__ASSERT_NO_MSG(size >= 0);

	K_SPINLOCK(&data->vuart_lock) {
		size = MIN(size, (int)tt_vuart_buf_size(vuart->rx_head, vuart->rx_tail));

		for (int i = 0; i < size; ++i) {
			(void)tt_vuart_poll_in(vuart, rx_data++, TT_VUART_ROLE_DEVICE);
		}
	}

	return size;
}

static void uart_tt_virt_irq_callback_set(const struct device *dev,
					  uart_irq_callback_user_data_t cb, void *user_data)
{
	struct uart_tt_virt_data *const data = dev->data;

	data->irq_cb = cb;
	data->irq_cb_udata = user_data;
}

static void uart_tt_virt_irq_err_disable(const struct device *dev)
{
	struct uart_tt_virt_data *const data = dev->data;

	K_SPINLOCK(&data->vuart_lock) {
		data->err_irq_en = false;
		if (!(data->rx_irq_en || data->tx_irq_en)) {
			/* If other interrupts are disabled, stop timer */
			k_timer_stop(&data->irq_timer);
		}
	}
}

static void uart_tt_virt_irq_err_enable(const struct device *dev)
{
	struct uart_tt_virt_data *const data = dev->data;

	K_SPINLOCK(&data->vuart_lock) {
		data->err_irq_en = true;
	}

	k_timer_start(&data->irq_timer, K_NO_WAIT, K_MSEC(CONFIG_UART_TT_VIRT_INTERRUPT_INTERVAL));
}

static void uart_tt_virt_irq_handler(struct k_timer *timer)
{
	struct uart_tt_virt_data *data = CONTAINER_OF(timer, struct uart_tt_virt_data, irq_timer);
	const struct device *dev = data->dev;
	uart_irq_callback_user_data_t cb = data->irq_cb;
	void *udata = data->irq_cb_udata;

	if (cb == NULL) {
		LOG_DBG("No IRQ callback configured for uart_tt_virt device %p", dev);
		return;
	}

	while (uart_tt_virt_irq_is_pending(dev)) {
		cb(dev, udata);
	}
}

static int uart_tt_virt_irq_is_pending(const struct device *dev)
{
	return uart_tt_virt_irq_tx_ready(dev) || uart_tt_virt_irq_rx_ready(dev);
}

static void uart_tt_virt_irq_rx_disable(const struct device *dev)
{
	struct uart_tt_virt_data *const data = dev->data;

	K_SPINLOCK(&data->vuart_lock) {
		data->rx_irq_en = false;
		if (!(data->tx_irq_en || data->err_irq_en)) {
			/* If other interrupts are disabled, stop timer */
			k_timer_stop(&data->irq_timer);
		}
	}
}

static void uart_tt_virt_irq_rx_enable(const struct device *dev)
{
	struct uart_tt_virt_data *const data = dev->data;

	K_SPINLOCK(&data->vuart_lock) {
		data->rx_irq_en = true;
	}

	k_timer_start(&data->irq_timer, K_NO_WAIT, K_MSEC(CONFIG_UART_TT_VIRT_INTERRUPT_INTERVAL));
}

static int uart_tt_virt_irq_rx_ready(const struct device *dev)
{
	int available = 0;
	struct uart_tt_virt_data *const data = dev->data;
	volatile struct tt_vuart *vuart = data->vuart;

	K_SPINLOCK(&data->vuart_lock) {
		if (!data->rx_irq_en) {
			K_SPINLOCK_BREAK;
		}

		available = !tt_vuart_buf_empty(vuart->rx_head, vuart->rx_tail);
	}

	return available;
}

static int uart_tt_virt_irq_tx_complete(const struct device *dev)
{
	bool tx_complete = false;
	struct uart_tt_virt_data *const data = dev->data;
	volatile struct tt_vuart *vuart = data->vuart;

	K_SPINLOCK(&data->vuart_lock) {
		tx_complete = tt_vuart_buf_empty(vuart->tx_head, vuart->tx_tail);
	}

	return tx_complete;
}

static void uart_tt_virt_irq_tx_disable(const struct device *dev)
{
	struct uart_tt_virt_data *const data = dev->data;

	K_SPINLOCK(&data->vuart_lock) {
		data->tx_irq_en = false;
		if (!(data->rx_irq_en || data->err_irq_en)) {
			/* If other interrupts are disabled, stop timer */
			k_timer_stop(&data->irq_timer);
		}
	}
}

static void uart_tt_virt_irq_tx_enable(const struct device *dev)
{
	struct uart_tt_virt_data *const data = dev->data;

	K_SPINLOCK(&data->vuart_lock) {
		data->tx_irq_en = true;
	}

	k_timer_start(&data->irq_timer, K_NO_WAIT, K_MSEC(CONFIG_UART_TT_VIRT_INTERRUPT_INTERVAL));
}

static int uart_tt_virt_irq_tx_ready(const struct device *dev)
{
	int available = 0;
	struct uart_tt_virt_data *const data = dev->data;
	volatile struct tt_vuart *vuart = data->vuart;

	K_SPINLOCK(&data->vuart_lock) {
		if (!data->tx_irq_en) {
			K_SPINLOCK_BREAK;
		}

		available = tt_vuart_buf_space(vuart->tx_head, vuart->tx_tail, vuart->tx_cap);
	}

	return available;
}

static void uart_tt_virt_irq_update(const struct device *dev)
{
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static int uart_tt_virt_poll_in(const struct device *dev, unsigned char *p_char)
{
	struct uart_tt_virt_data *data = dev->data;
	volatile struct tt_vuart *vuart = data->vuart;

	return (tt_vuart_poll_in(vuart, p_char, TT_VUART_ROLE_DEVICE) == -1) ? -1 : 0;
}

void uart_tt_virt_poll_out(const struct device *dev, unsigned char out_char)
{
	struct uart_tt_virt_data *data = dev->data;
	volatile struct tt_vuart *const vuart = data->vuart;

	tt_vuart_poll_out(vuart, out_char, TT_VUART_ROLE_DEVICE);
}

static DEVICE_API(uart, uart_tt_virt_api) = {
#ifdef CONFIG_UART_USE_RUNTIME_CONFIGURE
	.config_get = uart_tt_virt_config_get,
	.configure = uart_tt_virt_configure,
#endif /* CONFIG_UART_USE_RUNTIME_CONFIGURE */
	.err_check = uart_tt_virt_err_check,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = uart_tt_virt_fifo_fill,
	.fifo_read = uart_tt_virt_fifo_read,
	.irq_callback_set = uart_tt_virt_irq_callback_set,
	.irq_err_disable = uart_tt_virt_irq_err_disable,
	.irq_err_enable = uart_tt_virt_irq_err_enable,
	.irq_is_pending = uart_tt_virt_irq_is_pending,
	.irq_rx_disable = uart_tt_virt_irq_rx_disable,
	.irq_rx_enable = uart_tt_virt_irq_rx_enable,
	.irq_rx_ready = uart_tt_virt_irq_rx_ready,
	.irq_tx_complete = uart_tt_virt_irq_tx_complete,
	.irq_tx_disable = uart_tt_virt_irq_tx_disable,
	.irq_tx_enable = uart_tt_virt_irq_tx_enable,
	.irq_tx_ready = uart_tt_virt_irq_tx_ready,
	.irq_update = uart_tt_virt_irq_update,
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
	.poll_in = uart_tt_virt_poll_in,
	.poll_out = uart_tt_virt_poll_out,
};

__weak void uart_tt_virt_init_callback(const struct device *dev, size_t inst)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(inst);
}

__weak volatile struct tt_vuart *uart_tt_virt_lookup_callback(size_t inst)
{
	ARG_UNUSED(inst);

	return NULL;
}

volatile struct tt_vuart *uart_tt_virt_get(const struct device *dev)
{
	struct uart_tt_virt_data *data = dev->data;

	return data->vuart;
}

/*
 * A descriptor published by a previous boot stage may only be adopted if it lives where this
 * stage expects it to and describes the same buffer layout. Anything else (a cold boot, a stale
 * scratch register, a mismatched image) means the buffer contents cannot be trusted.
 */
static bool uart_tt_virt_adoptable(const struct uart_tt_virt_config *config,
				   volatile const struct tt_vuart *vuart)
{
	return (vuart == config->vuart) && (vuart->magic == config->magic) &&
	       (vuart->version == config->version) && (vuart->rx_cap == config->rx_cap) &&
	       (vuart->tx_cap == config->tx_cap);
}

static int uart_tt_virt_init(const struct device *dev)
{
	const struct uart_tt_virt_config *config = dev->config;
	struct uart_tt_virt_data *const data = dev->data;
	size_t inst = config->version >> 24;

	data->vuart = config->vuart;

	if (config->init_metadata ||
	    !uart_tt_virt_adoptable(config, uart_tt_virt_lookup_callback(inst))) {
		/* Populate header fields and drop whatever the buffer area held before */
		data->vuart->magic = config->magic;
		data->vuart->version = config->version;
		data->vuart->rx_cap = config->rx_cap;
		data->vuart->rx_head = 0;
		data->vuart->rx_tail = 0;
		data->vuart->tx_cap = config->tx_cap;
		data->vuart->tx_head = 0;
		data->vuart->tx_oflow = 0;
		data->vuart->tx_tail = 0;
	}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	data->dev = dev;
	k_timer_init(&data->irq_timer, uart_tt_virt_irq_handler, NULL);
#endif

	uart_tt_virt_init_callback(dev, inst);

	return 0;
}

#define UART_TT_VIRT_DESC_SIZE(_inst)                                                              \
	(sizeof(struct tt_vuart) + DT_INST_PROP(_inst, rx_cap) + DT_INST_PROP(_inst, tx_cap))

/* Descriptor placed in .bss, contents are lost when a new image takes over the processor */
#define UART_TT_VIRT_DEFINE_AREA(_inst)                                                            \
	struct uart_tt_virt_area_##_inst {                                                         \
		union {                                                                            \
			uint32_t mem[DIV_ROUND_UP(UART_TT_VIRT_DESC_SIZE(_inst),                   \
						  sizeof(uint32_t))];                              \
			struct tt_vuart vuart;                                                     \
		};                                                                                 \
	};                                                                                         \
	static struct uart_tt_virt_area_##_inst uart_tt_virt_area_##_inst;

/* Descriptor placed at a fixed address, contents survive across boot stages */
#define UART_TT_VIRT_CHECK_REGION(_inst)                                                           \
	BUILD_ASSERT(DT_REG_SIZE(DT_INST_PHANDLE(_inst, memory_region)) >=                         \
			     UART_TT_VIRT_DESC_SIZE(_inst),                                        \
		     "tenstorrent,vuart memory-region is too small for its buffers");

#define UART_TT_VIRT_PTR(_inst)                                                                    \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(_inst, memory_region),                                   \
		    ((struct tt_vuart *)(uintptr_t)DT_REG_ADDR(                                    \
			    DT_INST_PHANDLE(_inst, memory_region))),                               \
		    ((struct tt_vuart *)&uart_tt_virt_area_##_inst.vuart))

/* clang-format off */
#define DEFINE_UART_TT_VIRT(_inst)                                                                 \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(_inst, memory_region),                                   \
		    (UART_TT_VIRT_CHECK_REGION(_inst)), (UART_TT_VIRT_DEFINE_AREA(_inst)))         \
	static const struct uart_tt_virt_config uart_tt_virt_config_##_inst = {                    \
		.vuart = UART_TT_VIRT_PTR(_inst),                                                  \
		.init_metadata = DT_INST_PROP(_inst, init_metadata),                               \
		.loopback = DT_INST_PROP(_inst, loopback),                                         \
		.magic = DT_INST_PROP(_inst, magic),                                               \
		.version = ((DT_INST_REG_ADDR(_inst)) << 24) |                                     \
			   (DT_INST_PROP(_inst, version) & GENMASK(23, 0)),                        \
		.rx_cap = DT_INST_PROP(_inst, rx_cap),                                             \
		.tx_cap = DT_INST_PROP(_inst, tx_cap),                                             \
	};                                                                                         \
	static struct uart_tt_virt_data uart_tt_virt_data_##_inst;                                 \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(_inst, uart_tt_virt_init, PM_DEVICE_DT_INST_GET(_inst),              \
			      &uart_tt_virt_data_##_inst, &uart_tt_virt_config_##_inst,            \
			      PRE_KERNEL_1, CONFIG_SERIAL_INIT_PRIORITY, &uart_tt_virt_api);

/* clang-format on */
DT_INST_FOREACH_STATUS_OKAY(DEFINE_UART_TT_VIRT)
