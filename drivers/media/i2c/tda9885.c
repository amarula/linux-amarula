/*
 * Driver for the TDA9885 chip
 *
 * Copyright (c) 2015 Bticino S.p.A. <raffaele.recalcati@bticino.it>
 * Added device tree support
 * Copyright (c) 2011 Bticino S.p.A. <raffaele.recalcati@bticino.it>
 * Copyright (c) 2010 Rodolfo Giometti <giometti@linux.it>

 * The former driver of Rodolfo Giometti has been converted
 * to a subdev one.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/videodev2.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <media/v4l2-device.h>
//#include <media/v4l2-chip-ident.h>
#include <media/v4l2-ctrls.h>
#include <linux/gpio.h>
#include <linux/sysfs.h>
#include "tda9885.h"

#define DRIVER_VERSION	"3.0.0"

#define AFCWIN (1<<7)

#define SWITCHING_MODE_DEFAULT 1
#define ADJUST_MODE_DEFAULT 1
#define DATA_MODE_DEFAULT 1

static int debug; /* insmod parameter */
module_param(debug, int, S_IRUGO | S_IWUSR);
MODULE_LICENSE("GPL");

struct tda9885 {
	struct v4l2_subdev sd;
	struct tda9885_platform_data *pdata;
	int status;
};

static struct tda9885_platform_data tda9885_private;
static struct i2c_client *tda9885_client;
struct mutex tda9885_mutex;

static inline struct tda9885 *to_state(struct v4l2_subdev *sd)
{
	return container_of(sd, struct tda9885, sd);
}

static ssize_t tda9885_power_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", !gpio_get_value(tda9885_private.power));
}

int tda9885_power_on(int power)
{
	u8 i2c_buf[4];
	int ret;

	mutex_lock(&tda9885_mutex);
	switch (power) {
	case 0:
		gpio_set_value(tda9885_private.power, 0);
		mutex_unlock(&tda9885_mutex);
		return 1;
	break;
	case 1:
	default:
		gpio_set_value(tda9885_private.power, 1);
		msleep(10);

		i2c_buf[0] = 0;
		i2c_buf[1] = tda9885_private.switching_mode;
		i2c_buf[2] = tda9885_private.adjust_mode;
		i2c_buf[3] = tda9885_private.data_mode;


		/*
		 * This chip is very simple, just write first the base address
		 * and then all registers settings.
		 */
		ret = i2c_master_send(tda9885_client, i2c_buf,
				      ARRAY_SIZE(i2c_buf));
		ret = (ret == ARRAY_SIZE(i2c_buf)) ? 0 : ret;
		mutex_unlock(&tda9885_mutex);
		return 1;
	break;
	};

	return 0;
}
EXPORT_SYMBOL(tda9885_power_on);

static ssize_t tda9885_power_store(struct device *dev,
		struct device_attribute *attr, const  char *buf, size_t count)
{
	unsigned long val;
	if (kstrtoul(buf, 16, &val))
		return -EINVAL;

	return tda9885_power_on(val);
}

static ssize_t tda9885_status_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	u8 i2c_buf[4];
	int ret = gpio_get_value(tda9885_private.power);

	switch (ret) {
	case 0:
		ret = i2c_master_recv(tda9885_client, i2c_buf, 1);
		if (unlikely(ret != 1))
			dev_err(&tda9885_client->dev, "wanted %d bytes, got %d\n",
				1, ret);
		return sprintf(buf, "0x%x\n", i2c_buf[0]);
	break;
	case 1:
	default:
		v4l_info(tda9885_client,
			 "Switch it on for reading status byte");
		return -ENODEV;
	};
}

static DEVICE_ATTR(tda9885_power, S_IRUGO | S_IWUSR,
	tda9885_power_show, tda9885_power_store);
static DEVICE_ATTR(tda9885_status, S_IRUGO,
	tda9885_status_show, NULL);

static struct attribute *sysfs_attrs_tda[] = {
	&dev_attr_tda9885_power.attr,
	&dev_attr_tda9885_status.attr,
	NULL
};

static struct attribute_group m_tda9885 = {
	.name = "tda9885",
	.attrs = sysfs_attrs_tda,
};

static int tda9885_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	v4l2_dbg(1, debug, sd, "%s : ctrl->id = %d\n", __func__, ctrl->id);
	return 0;
}

static int tda9885_querystd(struct v4l2_subdev *sd, v4l2_std_id *std)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct tda9885 *t = to_state(sd);
	int ret;
	u8 status;
	u8 buf[] = {
		0, t->pdata->switching_mode, t->pdata->adjust_mode, t->pdata->data_mode,
	};

	v4l2_dbg(1, debug, sd, "Switching ON the demodulator\n");

	/*
	 * This chip is very simple, just write first the base address
	 * and then all registers settings.
	 */
	ret = i2c_master_send(client, buf, ARRAY_SIZE(buf));
	ret = (ret == ARRAY_SIZE(buf)) ? 0 : ret;
	v4l2_dbg(1, debug, sd, "Reading status byte\n");
	ret = i2c_master_recv(client, &status, 1);
	if (unlikely(ret != 1))
		dev_err(&client->dev, "wanted %d bytes, got %d\n",
			1, ret);
	v4l2_dbg(1, debug, sd, "Status byte 0x%02X\n", status);
	ret = 0;

	switch (status & AFCWIN) {
	case 1:
		*std = V4L2_STD_PAL;
	break;
	case 0:
	default:
	break;
	}

	return 0;
}

static int tda9885_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct tda9885 *t = to_state(sd);
	int ret;
	u8 buf[] = {
		0, t->pdata->switching_mode, t->pdata->adjust_mode, t->pdata->data_mode,
	};

	switch (enable) {
	case 0:
	{
		v4l2_dbg(1, debug, sd, "Switching OFF the demodulator\n");
		/* Power Down */
		gpio_set_value(t->pdata->power, 0);
		ret = 0;
		break;
	}
	case 1:
	{
		v4l2_dbg(1, debug, sd, "Switching ON the demodulator\n");
		/* Power up */
		gpio_set_value(t->pdata->power, 1); /* Alwasy ON */

		/*
		 * Little delay for power up
		 * datasheet: time constant (R × C) for network
		 * without i2c bus is 1.2 usec
		*/
		mdelay(1);

		/*
		 * This chip is very simple, just write first the base address
		 * and then all registers settings.
		 */
		ret = i2c_master_send(client, buf, ARRAY_SIZE(buf));
		ret = (ret == ARRAY_SIZE(buf)) ? 0 : ret;

		v4l2_dbg(1, debug, sd, "Reading status byte\n");
		ret = i2c_master_recv(client, buf, 1);
		if (unlikely(ret != 1))
			dev_err(&client->dev, "wanted %d bytes, got %d\n",
				1, ret);
		v4l2_dbg(1, debug, sd, "Status byte 0x%02X\n", buf[0]);
		ret = 0;
		break;
	}
	default:
		return -ENODEV;
		break;
	}

	return ret;
}

static int tda9885_s_power(struct v4l2_subdev *sd, int power)
{
	return tda9885_s_stream(sd, power);
}

static const struct v4l2_subdev_video_ops tda9885_video_ops = {
	.s_stream = tda9885_s_stream,
	.querystd = tda9885_querystd,
};

static const struct v4l2_subdev_core_ops tda9885_core_ops = {
//	.g_ctrl = tda9885_g_ctrl,
	.s_power = tda9885_s_power,
};

static const struct v4l2_subdev_ops tda9885_ops = {
	.core = &tda9885_core_ops,
	.video = &tda9885_video_ops
};

/*
 * I2C init/probing/exit functions
 */
static int tda9885_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct tda9885 *data;
	struct v4l2_subdev *sd;
	struct device_node *np = client->dev.of_node;
	struct tda9885_platform_data *pdata = dev_get_platdata(&client->dev);
	int err = 0;
	int ret;

	v4l_info(client, "chip found @ 1x%02x (%s)\n",
			client->addr << 1, client->adapter->name);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE)) {
		err = -EIO;
		goto exit;
	}

	if (!np && !client->dev.platform_data) {
		v4l2_err(client, "No platform data!!\n");
		return -ENODEV;
	}

	data = kzalloc(sizeof(struct tda9885), GFP_KERNEL);
	if (np) {
		pdata = devm_kzalloc(&client->dev,
					sizeof(struct tda9885_platform_data),
					GFP_KERNEL);

		pdata->switching_mode = SWITCHING_MODE_DEFAULT;
		of_property_read_u8(np, "nxp,switching_mode", &pdata->switching_mode);

		pdata->adjust_mode = ADJUST_MODE_DEFAULT;
		of_property_read_u8(np, "nxp,adjust_mode", &pdata->adjust_mode);

		pdata->data_mode = DATA_MODE_DEFAULT;
		of_property_read_u8(np, "nxp,data_mode", &pdata->data_mode);

		pdata->power = of_get_named_gpio(np, "power-gpio", 0);
		if (pdata->power >= 0)
		{
			ret = gpio_request(pdata->power, "tda9885 power_gpio");
			if (ret < 0)
				goto exit;
			gpio_direction_output(pdata->power, 0);
			gpio_set_value(pdata->power, 0);  /* OFF */
			gpio_export(pdata->power, 0);
		} else {
			pdata->power = -EINVAL;
			goto exit;
		}

		data->pdata = pdata;
	} else {
		/* Copy board specific information here */
		dev_err(&client->dev, "Platform data set without device tree\n");
		data->pdata = client->dev.platform_data;
	}

	i2c_set_clientdata(client, data);

	/* Register with V4L2 layer as slave device */
	sd = &data->sd;
	v4l2_i2c_subdev_init(sd, client, &tda9885_ops);
	v4l2_dbg(1, debug, sd, "default switching mode is 0x%02x\n",
		data->pdata->switching_mode);
	v4l2_dbg(1, debug, sd, "default adjust mode is 0x%02x\n",
		data->pdata->adjust_mode);
	v4l2_dbg(1, debug, sd, "default data mode is 0x%02x\n",
		data->pdata->data_mode);
	v4l2_dbg(1, debug, sd, "power gpio is %d\n",
		data->pdata->power);
	v4l2_info(sd, "%s decoder driver registered (ver. %s)\n", sd->name, DRIVER_VERSION);

	/* Saving context for sysfs direct management */
	memcpy(&tda9885_private, data->pdata,
	       sizeof(struct tda9885_platform_data));
	tda9885_client = client;

	ret = sysfs_create_group(&client->dev.kobj, &m_tda9885);
	if (ret) {
		dev_err(&client->dev, "device create file failed\n");
		return ret;
	}

	mutex_init(&tda9885_mutex);

	gpio_set_value(data->pdata->power, 0); /* Normally Off */

	pr_info("chip found @ 1x%02x (%s)\n",
			client->addr << 1, client->adapter->name);

	return 0;

exit:
	devm_kfree(&client->dev, pdata);
	return err;
}

static int tda9885_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct tda9885 *t = to_state(sd);

	v4l2_device_unregister_subdev(sd);

	sysfs_remove_group(&client->dev.kobj, &m_tda9885);

	gpio_set_value(t->pdata->power, 0);
	kfree(to_state(sd));
	return 0;
}

static const struct i2c_device_id tda9885_id[] = {
	{ "tda9885", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tda9885_id);

static const struct of_device_id adv7180_of_id[] = {
	{ .compatible = "tda,tda9885", },
	{ },
};

static struct i2c_driver tda9885_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "tda9885",
		.of_match_table = of_match_ptr(adv7180_of_id),
	},
	.probe		= tda9885_probe,
	.remove		= tda9885_remove,
	.id_table	= tda9885_id,
};

static __init int init_tda9885(void)
{
	return i2c_add_driver(&tda9885_driver);
}

static __exit void exit_tda9885(void)
{
	i2c_del_driver(&tda9885_driver);
}

module_init(init_tda9885);
module_exit(exit_tda9885);

/* Module information */
MODULE_AUTHOR("Rodolfo Giometti <giometti@linux.it>");
MODULE_DESCRIPTION("TDA9885 IF-PPL demodulator driver");
MODULE_LICENSE("GPLv2");
MODULE_VERSION(DRIVER_VERSION);

