/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#define DT_DRV_COMPAT tenstorrent_flash_mux

LOG_MODULE_REGISTER(flash_mux, CONFIG_FLASH_LOG_LEVEL);

struct flash_mux_config {
	const struct device *const *candidates;
	size_t num_candidates;
};

struct flash_mux_data {
	const struct device *active;
};

static const struct device *flash_mux_active(const struct device *dev)
{
	const struct flash_mux_data *data = dev->data;

	return data->active;
}

static int flash_mux_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	return flash_read(flash_mux_active(dev), offset, data, len);
}

static int flash_mux_write(const struct device *dev, off_t offset, const void *data, size_t len)
{
	return flash_write(flash_mux_active(dev), offset, data, len);
}

static int flash_mux_erase(const struct device *dev, off_t offset, size_t len)
{
	return flash_erase(flash_mux_active(dev), offset, len);
}

static const struct flash_parameters *flash_mux_get_parameters(const struct device *dev)
{
	return flash_get_parameters(flash_mux_active(dev));
}

static int flash_mux_get_size(const struct device *dev, uint64_t *size)
{
	return flash_get_size(flash_mux_active(dev), size);
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
static void flash_mux_page_layout(const struct device *dev,
				  const struct flash_pages_layout **layout, size_t *layout_size)
{
	const struct device *active = flash_mux_active(dev);
	const struct flash_driver_api *api = active->api;

	api->page_layout(active, layout, layout_size);
}
#endif

#if defined(CONFIG_FLASH_JESD216_API)
static int flash_mux_sfdp_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	return flash_sfdp_read(flash_mux_active(dev), offset, data, len);
}

static int flash_mux_read_jedec_id(const struct device *dev, uint8_t *id)
{
	return flash_read_jedec_id(flash_mux_active(dev), id);
}
#endif

#if defined(CONFIG_FLASH_EX_OP_ENABLED)
static int flash_mux_ex_op(const struct device *dev, uint16_t code, const uintptr_t in, void *out)
{
	return flash_ex_op(flash_mux_active(dev), code, in, out);
}
#endif

static int flash_mux_init(const struct device *dev)
{
	const struct flash_mux_config *config = dev->config;
	struct flash_mux_data *data = dev->data;

	for (size_t i = 0; i < config->num_candidates; i++) {
		const struct device *candidate = config->candidates[i];

		if (device_is_ready(candidate)) {
			LOG_INF("Selected %s", candidate->name);
			data->active = candidate;
			return 0;
		}
	}

	LOG_ERR("No flash candidate initialized");
	return -ENODEV;
}

static DEVICE_API(flash, flash_mux_api) = {
	.read = flash_mux_read,
	.write = flash_mux_write,
	.erase = flash_mux_erase,
	.get_parameters = flash_mux_get_parameters,
	.get_size = flash_mux_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = flash_mux_page_layout,
#endif
#if defined(CONFIG_FLASH_JESD216_API)
	.sfdp_read = flash_mux_sfdp_read,
	.read_jedec_id = flash_mux_read_jedec_id,
#endif
#if defined(CONFIG_FLASH_EX_OP_ENABLED)
	.ex_op = flash_mux_ex_op,
#endif
};

#define FLASH_MUX_CANDIDATE(node_id, prop, idx)                                                    \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define FLASH_MUX_DEFINE(inst)                                                                     \
	static const struct device *const flash_mux_candidates_##inst[] = {                        \
		DT_INST_FOREACH_PROP_ELEM(inst, flash_devices, FLASH_MUX_CANDIDATE)};              \
	static const struct flash_mux_config flash_mux_config_##inst = {                           \
		.candidates = flash_mux_candidates_##inst,                                         \
		.num_candidates = ARRAY_SIZE(flash_mux_candidates_##inst),                         \
	};                                                                                         \
	static struct flash_mux_data flash_mux_data_##inst;                                        \
	DEVICE_DT_INST_DEFINE(inst, flash_mux_init, NULL, &flash_mux_data_##inst,                  \
			      &flash_mux_config_##inst, POST_KERNEL,                               \
			      CONFIG_FLASH_TT_MUX_INIT_PRIORITY, &flash_mux_api);

DT_INST_FOREACH_STATUS_OKAY(FLASH_MUX_DEFINE)
