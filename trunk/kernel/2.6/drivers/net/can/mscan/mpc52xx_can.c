/*
 * $Id:$
 *
 * DESCRIPTION:
 *  CAN bus driver for the Freescale MPC52xx embedded CPU.
 *
 * AUTHOR:
 *  Andrey Volkov <avolkov@varma-el.com>
 *
 * COPYRIGHT:
 *  2004-2005, Varma Electronics Oy
 *
 * LICENCE:
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * HISTORY:
 *	 2005-02-03 created
 *
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/can/can.h>
#include <can/can_device.h>
#include <can/mscan/mscan.h>
#include <asm/io.h>
#include <asm/mpc52xx.h>

static int __devinit mpc52xx_can_probe(struct platform_device *pdev)
{
	struct can_device *can;
	struct resource *mem;
	struct net_device *ndev;
	struct mscan_platform_data *pdata = pdev->dev.platform_data;
	u32 mem_size;
	int ret = -ENODEV;

	if(!pdata)
		return ret;

	can = alloc_mscandev();
	if (!can)
		return -ENOMEM;

	ndev = CAN2ND(can);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ndev->irq = platform_get_irq(pdev, 0);
	if (!mem || !ndev->irq)
		goto req_error;

	mem_size = mem->end - mem->start + 1;
	if (!request_mem_region(mem->start, mem_size, pdev->dev.driver->name)) {
		dev_err(&pdev->dev, "resource unavailable\n");
		goto req_error;
	}

	SET_NETDEV_DEV(ndev, &pdev->dev);
	SET_MODULE_OWNER(THIS_MODULE);

	ndev->base_addr = (unsigned long)ioremap_nocache(mem->start, mem_size);

	if (!ndev->base_addr) {
		dev_err(&pdev->dev, "failed to map can port\n");
		ret = -ENOMEM;
		goto fail_map;
	}

	can->can_sys_clock = pdata->clock_frq;

	platform_set_drvdata(pdev, can);

	ret = mscan_register(can, pdata->clock_src);
	if (ret >= 0) {
		dev_info(&pdev->dev, "probe for a port 0x%lX done\n", ndev->base_addr);
		return ret;
	}

	iounmap((unsigned long *)ndev->base_addr);
fail_map:
	release_mem_region(mem->start, mem_size);
req_error:
	free_candev(can);
	dev_err(&pdev->dev, "probe failed\n");
	return ret;
}

static int __devexit mpc52xx_can_remove(struct platform_device *pdev)
{
	struct can_device *can = platform_get_drvdata(pdev);
	struct resource *mem;

	platform_set_drvdata(pdev, NULL);
	mscan_unregister(can);

	iounmap((volatile void __iomem *)(CAN2ND(can)->base_addr));
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(mem->start, mem->start - mem->end + 1);
	free_candev(can);
	return 0;
}

static struct platform_driver mpc52xx_can_driver = {
	.driver		= {
		.name	  = "mpc52xx-mscan",
	},
	.probe 	  = mpc52xx_can_probe,
	.remove   = __devexit_p(mpc52xx_can_remove)
#ifdef CONFIG_PM
/*	.suspend	= mpc52xx_can_suspend,	TODO */
/*	.resume		= mpc52xx_can_resume,	TODO */
#endif
};

int __init mpc52xx_can_init(void)
{
	printk(KERN_INFO "%s initializing\n", mpc52xx_can_driver.driver.name);
	return platform_driver_register(&mpc52xx_can_driver);
}

void __exit mpc52xx_can_exit(void)
{
 	platform_driver_unregister(&mpc52xx_can_driver);
	printk(KERN_INFO "%s unloaded\n", mpc52xx_can_driver.driver.name);
}


module_init(mpc52xx_can_init);
module_exit(mpc52xx_can_exit);

MODULE_AUTHOR("Andrey Volkov <avolkov@varma-el.com>");
MODULE_DESCRIPTION("Freescale MPC5200 CAN driver");
MODULE_LICENSE("GPLv2");
