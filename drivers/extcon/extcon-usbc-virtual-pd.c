// SPDX-License-Identifier: GPL-2.0-only
/*
 * Virtual Type-C PD Extcon driver
 *
 * Copyright (c) 2019 Fuzhou Rockchip Electronics Co., Ltd
 * Copyright (c) 2019 Amarula Solutions(India)
 */

#include <linux/extcon-provider.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/delay.h>

struct virtual_pd {
	struct extcon_dev *extcon;
	struct device *dev;

	struct gpio_desc *gpio_irq;
	bool flip;
	bool usb_ss;
	bool enable;
	u8 mode;
	int irq;
	int enable_irq;
	u8 plug_state;
	struct work_struct work;
	spinlock_t irq_lock;
	struct delayed_work irq_work;
	int shake_lev;
};

static const unsigned int vpd_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_DISP_DP,
	EXTCON_NONE,
};

enum vpd_data_roles {
	VPD_DFP = 0,
	VPD_UFP,
	VPD_DP,
	VPD_DP_UFP,
};

static void vpd_extcon_notify(struct virtual_pd *vpd, bool flip, bool usb_ss,
			      bool dfp, bool ufp, bool dp)
{
	union extcon_property_value property;

	extcon_set_state(vpd->extcon, EXTCON_USB, ufp);
	extcon_set_state(vpd->extcon, EXTCON_USB_HOST, dfp);
	extcon_set_state(vpd->extcon, EXTCON_DISP_DP, dp);

	property.intval = flip;
	extcon_set_property(vpd->extcon, EXTCON_USB,
			    EXTCON_PROP_USB_VBUS, property);
	extcon_set_property(vpd->extcon, EXTCON_USB_HOST,
			    EXTCON_PROP_USB_VBUS, property);
	extcon_set_property(vpd->extcon, EXTCON_USB,
			    EXTCON_PROP_USB_TYPEC_POLARITY, property);
	extcon_set_property(vpd->extcon, EXTCON_USB_HOST,
			    EXTCON_PROP_USB_TYPEC_POLARITY, property);
	extcon_set_property(vpd->extcon, EXTCON_DISP_DP,
			    EXTCON_PROP_USB_TYPEC_POLARITY, property);

	property.intval = usb_ss;
	extcon_set_property(vpd->extcon, EXTCON_USB,
			    EXTCON_PROP_USB_SS, property);
	extcon_set_property(vpd->extcon, EXTCON_USB_HOST,
			    EXTCON_PROP_USB_SS, property);
	extcon_set_property(vpd->extcon, EXTCON_DISP_DP,
			    EXTCON_PROP_USB_SS, property);

	extcon_sync(vpd->extcon, EXTCON_USB);
	extcon_sync(vpd->extcon, EXTCON_USB_HOST);
	extcon_sync(vpd->extcon, EXTCON_DISP_DP);
}

static void vpd_extcon_notify_set(struct virtual_pd *vpd)
{
	bool flip = vpd->flip, usb_ss = vpd->usb_ss;
	bool dfp = 0, ufp = 0, dp = 0;

	switch (vpd->mode) {
	case VPD_DFP:
		dfp = 1;
		break;
	case VPD_DP:
		dp = 1;
		dfp = 1;
		break;
	case VPD_DP_UFP:
		dp = 1;
		ufp = 1;
		break;
	case VPD_UFP:
		/* fall through */
	default:
		ufp = 1;
		break;
	}

	vpd_extcon_notify(vpd, flip, usb_ss, dfp, ufp, dp);
}

static void vpd_extcon_notify_clr(struct virtual_pd *vpd)
{
	vpd_extcon_notify(vpd, vpd->flip, vpd->usb_ss, 0, 0, 0);
}

void vpd_irq_disable(struct virtual_pd *vpd)
{
	unsigned long irqflags = 0;

	spin_lock_irqsave(&vpd->irq_lock, irqflags);
	if (!vpd->enable_irq) {
		disable_irq_nosync(vpd->irq);
		vpd->enable_irq = 1;
	} else {
		dev_warn(vpd->dev, "irq have already disabled\n");
	}
	spin_unlock_irqrestore(&vpd->irq_lock, irqflags);
}

void vpd_irq_enable(struct virtual_pd *vpd)
{
	unsigned long irqflags = 0;

	spin_lock_irqsave(&vpd->irq_lock, irqflags);
	if (vpd->enable_irq) {
		enable_irq(vpd->irq);
		vpd->enable_irq = 0;
	}
	spin_unlock_irqrestore(&vpd->irq_lock, irqflags);
}

static void extcon_pd_delay_irq_work(struct work_struct *work)
{
	struct virtual_pd *vpd = container_of(work, struct virtual_pd, irq_work.work);
	int lev = gpiod_get_raw_value(vpd->gpio_irq);

	if(vpd->shake_lev != lev) {
		vpd_irq_enable(vpd);
		return;
	}

	switch(vpd->plug_state) {
		case 1:
			if(lev==0) {
				vpd->enable = false;
				vpd_extcon_notify_clr(vpd);
				vpd->plug_state=0;
			}
			break;
		case 0:
			if(lev==1) {
				vpd->enable = true;
				vpd_extcon_notify_set(vpd);
				vpd->plug_state=1;
			}
			break;
		default:
			break;
	}
	vpd_irq_enable(vpd);
}

static irqreturn_t dp_det_irq_handler(int irq, void *dev_id)
{
	struct virtual_pd *vpd = dev_id;
	int lev;

	lev = gpiod_get_raw_value(vpd->gpio_irq);
	vpd->shake_lev = lev;
	schedule_delayed_work(&vpd->irq_work, msecs_to_jiffies(10));
	vpd_irq_disable(vpd);

	return IRQ_HANDLED;
}

static void vpd_extcon_init(struct virtual_pd *vpd)
{
	struct device *dev = vpd->dev;
	u32 tmp = 0;
	int ret = 0;

	ret = device_property_read_u32(dev, "vpd,init-flip", &tmp);
	if (ret < 0)
		vpd->flip = 0;
	else
		vpd->flip = tmp;
	dev_dbg(dev, "init-flip = %d\n", vpd->flip);

	ret = device_property_read_u32(dev, "vpd,init-ss", &tmp);
	if (ret < 0)
		vpd->usb_ss = 0;
	else
		vpd->usb_ss = tmp;
	dev_dbg(dev, "init-ss = %d\n", vpd->usb_ss);

	ret = device_property_read_u32(dev, "vpd,init-mode", &tmp);
	if (ret < 0)
		vpd->mode = 0;
	else
		vpd->mode = tmp;
	dev_dbg(dev, "init-mode = %d\n", vpd->mode);
	if(gpiod_get_raw_value(vpd->gpio_irq)) {
		vpd_extcon_notify_set(vpd);
		vpd->plug_state=1;
	}
}

static int vpd_extcon_probe(struct platform_device *pdev)
{
	struct virtual_pd *vpd;
	struct device *dev = &pdev->dev;
	int ret = 0;

	dev_info(dev, "%s: %d start\n", __func__, __LINE__);

	vpd = devm_kzalloc(dev, sizeof(*vpd), GFP_KERNEL);
	if (!vpd)
		return -ENOMEM;

	vpd->dev = dev;
	dev_set_drvdata(dev, vpd);
	vpd->enable = 1;

	vpd->extcon = devm_extcon_dev_allocate(dev, vpd_cable);
	if (IS_ERR(vpd->extcon)) {
		dev_err(dev, "allocat extcon failed\n");
		return PTR_ERR(vpd->extcon);
	}

	ret = devm_extcon_dev_register(dev, vpd->extcon);
	if (ret) {
		dev_err(dev, "register extcon failed: %d\n", ret);
		return ret;
	}

	vpd->gpio_irq = devm_gpiod_get_optional(dev,"det",
						    GPIOD_OUT_LOW);

	if (IS_ERR(vpd->gpio_irq)) {
		dev_warn(dev, "maybe miss named GPIO for dp-det\n");
		vpd->gpio_irq = NULL;
	}

	extcon_set_property_capability(vpd->extcon, EXTCON_USB, EXTCON_PROP_USB_VBUS);
	extcon_set_property_capability(vpd->extcon, EXTCON_USB_HOST, EXTCON_PROP_USB_VBUS);
	ret = extcon_set_property_capability(vpd->extcon, EXTCON_USB,
					     EXTCON_PROP_USB_TYPEC_POLARITY);
	if (ret) {
		dev_err(dev,
			"set USB property capability failed: %d\n", ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_USB_HOST,
					     EXTCON_PROP_USB_TYPEC_POLARITY);
	if (ret) {
		dev_err(dev,
			"set USB_HOST property capability failed: %d\n",
			ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_DISP_DP,
					     EXTCON_PROP_USB_TYPEC_POLARITY);
	if (ret) {
		dev_err(dev,
			"set DISP_DP property capability failed: %d\n",
			ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_USB,
					     EXTCON_PROP_USB_SS);
	if (ret) {
		dev_err(dev,
			"set USB USB_SS property capability failed: %d\n",
			ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_USB_HOST,
					     EXTCON_PROP_USB_SS);
	if (ret) {
		dev_err(dev,
			"set USB_HOST USB_SS property capability failed: %d\n",
			ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_DISP_DP,
					     EXTCON_PROP_USB_SS);
	if (ret) {
		dev_err(dev,
			"set DISP_DP USB_SS property capability failed: %d\n",
			ret);
		return ret;
	}

	vpd_extcon_init(vpd);
	INIT_DELAYED_WORK(&vpd->irq_work, extcon_pd_delay_irq_work);

	vpd->irq=gpiod_to_irq(vpd->gpio_irq);
	if (vpd->irq){
		ret = devm_request_threaded_irq(dev,
                        vpd->irq,
                        NULL,
                        dp_det_irq_handler,
                        //IRQF_TRIGGER_HIGH | IRQF_ONESHOT ,
                        IRQF_TRIGGER_FALLING |IRQF_TRIGGER_RISING | IRQF_ONESHOT ,
                        NULL,
                        vpd);
	}
	else
		dev_err(dev,"gpio can not be irq !\n");

	dev_info(dev, "%s: %d success\n", __func__, __LINE__);

	return 0;
}

static int vpd_extcon_remove(struct platform_device *pdev)
{
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int vpd_extcon_suspend(struct device *dev)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	int lev=0;
	lev = gpiod_get_raw_value(vpd->gpio_irq);
	cancel_delayed_work_sync(&vpd->irq_work);
	vpd_irq_disable(vpd);
	return 0;
}

static int vpd_extcon_resume(struct device *dev)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);
	vpd_irq_enable(vpd);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(vpd_extcon_pm_ops,
			 vpd_extcon_suspend, vpd_extcon_resume);

static const struct of_device_id vpd_extcon_dt_match[] = {
	{ .compatible = "linux,extcon-virtual-pd", },
	{ /* sentinel */ }
};

static struct platform_driver vpd_extcon_driver = {
	.probe		= vpd_extcon_probe,
	.remove		= vpd_extcon_remove,
	.driver		= {
		.name	= "extcon-virtual-pd",
		.pm	= &vpd_extcon_pm_ops,
		.of_match_table = vpd_extcon_dt_match,
	},
};

module_platform_driver(vpd_extcon_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("rockchip");
MODULE_DESCRIPTION("Virtual Typec-pd extcon driver");
