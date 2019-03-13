// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Amarula Solutions
 * Author: Jagan Teki <jagan@amarulasolutions.com>
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <drm/drmP.h>

#include <linux/gpio/consumer.h>
#include <linux/of_graph.h>

struct icn6211 {
	struct device *dev;
	struct drm_bridge bridge;
	struct drm_connector connector;
	struct drm_panel *panel;

	struct gpio_desc *reset;
};

static inline struct icn6211 *bridge_to_icn6211(struct drm_bridge *bridge)
{
	return container_of(bridge, struct icn6211, bridge);
}

static inline
struct icn6211 *connector_to_icn6211(struct drm_connector *connector)
{
	return container_of(connector, struct icn6211, connector);
}

struct icn6211_init_cmd {
	size_t len;
	const char *data;
};

#define ICN6211_INIT_CMD(...) { \
	.len = sizeof((char[]){__VA_ARGS__}), \
	.data = (char[]){__VA_ARGS__} }

static const struct icn6211_init_cmd icn6211_init_cmds[] = {
	ICN6211_INIT_CMD(0x7A, 0xC1),
	ICN6211_INIT_CMD(0x20, 0x20),
	ICN6211_INIT_CMD(0x21, 0xE0),
	ICN6211_INIT_CMD(0x22, 0x13),
	ICN6211_INIT_CMD(0x23, 0x28),
	ICN6211_INIT_CMD(0x24, 0x30),
	ICN6211_INIT_CMD(0x25, 0x28),
	ICN6211_INIT_CMD(0x26, 0x00),
	ICN6211_INIT_CMD(0x27, 0x0D),
	ICN6211_INIT_CMD(0x28, 0x03),
	ICN6211_INIT_CMD(0x29, 0x1D),
	ICN6211_INIT_CMD(0x34, 0x80),
	ICN6211_INIT_CMD(0x36, 0x28),
	ICN6211_INIT_CMD(0xB5, 0xA0),
	ICN6211_INIT_CMD(0x5C, 0xFF),
	ICN6211_INIT_CMD(0x2A, 0x01),
	ICN6211_INIT_CMD(0x56, 0x92),
	ICN6211_INIT_CMD(0x6B, 0x71),
	ICN6211_INIT_CMD(0x69, 0x2B),
	ICN6211_INIT_CMD(0x10, 0x40),
	ICN6211_INIT_CMD(0x11, 0x98),
	ICN6211_INIT_CMD(0xB6, 0x20),
	ICN6211_INIT_CMD(0x51, 0x20),
	ICN6211_INIT_CMD(0x09, 0x10),
};

static int icn6211_get_modes(struct drm_connector *connector)
{
	struct icn6211 *ctx = connector_to_icn6211(connector);

	return drm_panel_get_modes(ctx->panel);
}

static const
struct drm_connector_helper_funcs icn6211_connector_helper_funcs = {
	.get_modes = icn6211_get_modes,
};

static const struct drm_connector_funcs icn6211_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void icn6211_disable(struct drm_bridge *bridge)
{
	struct icn6211 *ctx = bridge_to_icn6211(bridge);
	int ret = drm_panel_disable(bridge_to_icn6211(bridge)->panel);

	if (ret < 0)
		dev_err(ctx->dev, "error disabling panel (%d)\n", ret);
}

static void icn6211_post_disable(struct drm_bridge *bridge)
{
	struct icn6211 *ctx = bridge_to_icn6211(bridge);
	int ret;

	ret = drm_panel_unprepare(ctx->panel);
	if (ret < 0)
		dev_err(ctx->dev, "error unpreparing panel (%d)\n", ret);

	gpiod_set_value(ctx->reset, 0);
	msleep(50);

	gpiod_set_value(ctx->reset, 1);
	msleep(50);

	gpiod_set_value(ctx->reset, 0);
	msleep(20);
}

static void icn6211_pre_enable(struct drm_bridge *bridge)
{
	struct icn6211 *ctx = bridge_to_icn6211(bridge);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	unsigned int i;
	int ret;

	gpiod_set_value(ctx->reset, 1);
	msleep(50);

	gpiod_set_value(ctx->reset, 0);
	msleep(50);

	gpiod_set_value(ctx->reset, 1);
	msleep(20);

	for (i = 0; i < ARRAY_SIZE(icn6211_init_cmds); i++) {
		const struct icn6211_init_cmd *cmd = &icn6211_init_cmds[i];

		ret = mipi_dsi_generic_write(dsi, cmd->data, cmd->len);
		if (ret < 0)
			return;

		msleep(10);
	}

	ret = drm_panel_prepare(ctx->panel);
	if (ret < 0)
		dev_err(ctx->dev, "error preparing panel (%d)\n", ret);
}

static void icn6211_enable(struct drm_bridge *bridge)
{
	struct icn6211 *ctx = bridge_to_icn6211(bridge);
	int ret = drm_panel_enable(ctx->panel);

	if (ret < 0)
		dev_err(ctx->dev, "error enabling panel (%d)\n", ret);
}

static int icn6211_attach(struct drm_bridge *bridge)
{
	struct icn6211 *ctx = bridge_to_icn6211(bridge);
	struct drm_device *drm = bridge->dev;
	int ret;

	ctx->connector.polled = DRM_CONNECTOR_POLL_HPD;
	ret = drm_connector_init(drm, &ctx->connector,
				 &icn6211_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret) {
		DRM_ERROR("Failed to initialize connector\n");
		return ret;
	}

	drm_connector_helper_add(&ctx->connector,
				 &icn6211_connector_helper_funcs);
	drm_connector_attach_encoder(&ctx->connector, bridge->encoder);
	drm_panel_attach(ctx->panel, &ctx->connector);
	ctx->connector.funcs->reset(&ctx->connector);
	drm_fb_helper_add_one_connector(drm->fb_helper, &ctx->connector);
	drm_connector_register(&ctx->connector);

	return 0;
}

static void icn6211_detach(struct drm_bridge *bridge)
{
	struct icn6211 *ctx = bridge_to_icn6211(bridge);
	struct drm_device *drm = bridge->dev;

	drm_connector_unregister(&ctx->connector);
	drm_fb_helper_remove_one_connector(drm->fb_helper, &ctx->connector);
	drm_panel_detach(ctx->panel);
	ctx->panel = NULL;
	drm_connector_put(&ctx->connector);
}

static const struct drm_bridge_funcs icn6211_bridge_funcs = {
	.disable = icn6211_disable,
	.post_disable = icn6211_post_disable,
	.enable = icn6211_enable,
	.pre_enable = icn6211_pre_enable,
	.attach = icn6211_attach,
	.detach = icn6211_detach,
};

static int icn6211_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct icn6211 *ctx;
	int ret;

	printk("%s\n", __func__);

	ctx = devm_kzalloc(dev, sizeof(struct icn6211), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_SYNC_PULSE;

	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset)) {
		dev_err(dev, "no reset GPIO pin provided\n");
		return PTR_ERR(ctx->reset);
	}

	ret = drm_of_find_panel_or_bridge(ctx->dev->of_node, 0, 0,
					  &ctx->panel, NULL);
	if (ret && ret != -EPROBE_DEFER) {
		dev_err(dev, "failed to find panel (ret = %d)\n", ret); 
		return ret;
	}

	printk("icn6211_probe: Try to attach bridge\n");
	ctx->bridge.funcs = &icn6211_bridge_funcs;
	ctx->bridge.of_node = dev->of_node;

	drm_bridge_add(&ctx->bridge);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_bridge_remove(&ctx->bridge);
		dev_err(dev, "failed to attach dsi (ret = %d)\n", ret);
	}

	printk("%s done! (ret = %d)\n", __func__, ret);
	return ret;
}

static int icn6211_remove(struct mipi_dsi_device *dsi)
{
	struct icn6211 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_bridge_remove(&ctx->bridge);

	return 0;
}

static const struct of_device_id icn6211_of_match[] = {
	{ .compatible = "bananapi,icn6211" },
	{ }
};
MODULE_DEVICE_TABLE(of, icn6211_of_match);

static struct mipi_dsi_driver icn6211_driver = {
	.probe = icn6211_probe,
	.remove = icn6211_remove,
	.driver = {
		.name = "bananpi-icn6211",
		.owner = THIS_MODULE,
		.of_match_table = icn6211_of_match,
	},
};
module_mipi_dsi_driver(icn6211_driver);

MODULE_AUTHOR("Jagan Teki <jagan@amarulasolutions.com>");
MODULE_DESCRIPTION("Bananapi ICN6211 MIPI-DSI to RGB Bridge");
MODULE_LICENSE("GPL v2");
