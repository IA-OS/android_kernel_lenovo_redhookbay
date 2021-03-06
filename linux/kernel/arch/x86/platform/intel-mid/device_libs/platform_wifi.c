/*
 * platform_bcm43xx.c: bcm43xx platform data initilization file
 *
 * (C) Copyright 2011 Intel Corporation
 * Author:
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/gpio.h>
#include <linux/lnw_gpio.h>
#include <asm/intel-mid.h>
#include <linux/wlan_plat.h>
#include <linux/interrupt.h>
#include <linux/mmc/sdhci.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include "pci/platform_sdhci_pci.h"
#include "platform_wifi.h"

static struct resource wifi_res[] = {
	{
	.name = "wlan_irq",
	.start = 1,
	.end = 1,
	.flags = IORESOURCE_IRQ | IRQF_TRIGGER_FALLING ,
	},
};

static struct platform_device wifi_device = {
	.name = "wlan",
	.dev = {
		.platform_data = NULL,
		},
	.num_resources = ARRAY_SIZE(wifi_res),
	.resource = wifi_res,
};

void __init wifi_platform_data_init_sfi(void)
{
	int err;
	int wifi_irq_gpio = -1;

	pr_err("wifi_platform_data_init_sfi\n");

	/*Get GPIO numbers from the SFI table*/
	wifi_irq_gpio = get_gpio_by_name(WIFI_SFI_GPIO_IRQ_NAME);
	if (wifi_irq_gpio < 0) {
		pr_err("%s: Unable to find" WIFI_SFI_GPIO_IRQ_NAME
		       "WLAN-interrupt GPIO in the SFI table\n",
		       __func__);
		return;
	}

	wifi_res[0].start = wifi_res[0].end = wifi_irq_gpio;

	/* For SFI compliant platforms, we need to manually register
	a platform device */

	err = platform_device_register(&wifi_device);
	if (err < 0)
		pr_err("platform_device_register failed for wifi_device\n");
}


/* Called from board.c */
void __init *wifi_platform_data(void *info)
{
	struct sd_board_info *sd_info = info;

	unsigned int sdhci_quirk = SDHCI_QUIRK2_ADVERTISE_2V0_FORCE_1V8
		| SDHCI_QUIRK2_ENABLE_MMC_PM_IGNORE_PM_NOTIFY
		| SDHCI_QUIRK2_ADVERTISE_3V0_FORCE_1V8
		| SDHCI_QUIRK2_NON_STD_CIS;

	pr_err("Using generic wifi platform data\n");

	/* Set vendor specific SDIO quirks */
	sdhci_pdata_set_quirks(sdhci_quirk);

#ifndef CONFIG_ACPI
	/* We are SFI here, register platform device */
	wifi_platform_data_init_sfi();
#endif

	return &wifi_device;
}
