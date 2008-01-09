/*
 * $Id$
 *
 * Copyright (C) 2005 Marc Kleine-Budde, Pengutronix
 * Copyright (C) 2006 Andrey Volkov, Varma Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the version 2 of the GNU General Public License
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <linux/can.h>
#include <linux/can/dev.h>

#include "sysfs.h"

MODULE_DESCRIPTION("CAN netdevice library");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Marc Kleine-Budde <mkl@pengutronix.de>, "
	      "Andrey Volkov <avolkov@varma-el.com>");

/*
 * Abstract:
 *   Bit rate calculated with next formula:
 *   bitrate = frq/(brp*(1 + prop_seg+ phase_seg1 + phase_seg2))
 *
 *   This calc function based on work of Florian Hartwich and Armin Bassemi
 *   "The Configuration of the CAN Bit Timing"
 *   (http://www.semiconductors.bosch.de/pdf/CiA99Paper.pdf)
 *
 *  Parameters:
 *  [in]
 *    bittime_nsec - expected bit time in nanosecs
 *
 *  [out]
 *    bittime      - calculated time segments, for meaning of
 * 		     each field read CAN standard.
 */

#define DEFAULT_MAX_BRP	64U
#define DEFAULT_MAX_SJW	4U

/* All below values in tq units */
#define MAX_BITTIME	25U
#define MIN_BITTIME	8U
#define MAX_PROP_SEG	8U
#define MAX_PHASE_SEG1	8U
#define MAX_PHASE_SEG2	8U

int can_calc_bittime(struct can_priv *can, u32 bitrate,
		     struct can_bittime_std *bittime)
{
	int best_error = -1;	/* Ariphmetic error */
	int df, best_df = -1;	/* oscillator's tolerance range,
				   greater is better */
	u32 quanta;		/* in tq units */
	u32 brp, phase_seg1, phase_seg2, sjw, prop_seg;
	u32 brp_min, brp_max, brp_expected;
	u64 tmp;

	/* bitrate range [1baud,1MiB/s] */
	if (bitrate == 0 || bitrate > 1000000UL)
		return -EINVAL;

	tmp = (u64) can->can_sys_clock * 1000;
	do_div(tmp, bitrate);
	brp_expected = (u32) tmp;

	brp_min = brp_expected / (1000 * MAX_BITTIME);
	if (brp_min == 0)
		brp_min = 1;
	if (brp_min > can->max_brp)
		return -ERANGE;

	brp_max = (brp_expected + 500 * MIN_BITTIME) / (1000 * MIN_BITTIME);
	if (brp_max == 0)
		brp_max = 1;
	if (brp_max > can->max_brp)
		brp_max = can->max_brp;

	for (brp = brp_min; brp <= brp_max; brp++) {
		quanta = brp_expected / (brp * 1000);
		if (quanta < MAX_BITTIME
		    && quanta * brp * 1000 != brp_expected)
			quanta++;
		if (quanta < MIN_BITTIME || quanta > MAX_BITTIME)
			continue;

		phase_seg2 = min((quanta - 3) / 2, MAX_PHASE_SEG2);
		for (sjw = can->max_sjw; sjw > 0; sjw--) {
			for (; phase_seg2 > sjw; phase_seg2--) {
				u32 err1, err2;
				phase_seg1 =
				    phase_seg2 % 2 ? phase_seg2 -
				    1 : phase_seg2;
				prop_seg = quanta - 1 - phase_seg2 - phase_seg1;
				/*
				 * FIXME: support of longer lines (i.e. bigger
				 * prop_seg) is more prefered than support of
				 * cheap oscillators (i.e. bigger
				 * df/phase_seg1/phase_seg2)
				 */
				if (prop_seg < phase_seg1)
					continue;
				if (prop_seg > MAX_PROP_SEG)
					goto next_brp;

				err1 = phase_seg1 * brp * 500 * 1000 /
				    (13 * brp_expected -
				     phase_seg2 * brp * 1000);
				err2 = sjw * brp * 50 * 1000 / brp_expected;

				df = min(err1, err2);
				if (df >= best_df) {
					unsigned error =
						abs(brp_expected * 10 /
						    (brp * (1 + prop_seg +
							    phase_seg1 +
							    phase_seg2)) -
						    10000);

					if (error > 10 || error > best_error)
						continue;

					if (error == best_error
					    && prop_seg < bittime->prop_seg)
						continue;

					best_error = error;
					best_df = df;
					bittime->brp = brp;
					bittime->prop_seg = prop_seg;
					bittime->phase_seg1 = phase_seg1;
					bittime->phase_seg2 = phase_seg2;
					bittime->sjw = sjw;
					bittime->sam =
						(bittime->phase_seg1 > 3);
				}
			}
		}
	      next_brp:;
	}

	if (best_error < 0)
		return -EDOM;
	return 0;
}
EXPORT_SYMBOL(can_calc_bittime);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
static struct net_device_stats *can_get_stats(struct net_device *dev)
{
	struct can_priv *priv = netdev_priv(dev);

	return &priv->net_stats;
}
#endif

static void can_setup(struct net_device *dev)
{
	dev->type = ARPHRD_CAN;
	dev->mtu = sizeof(struct can_frame);
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->tx_queue_len = 10;

	/* New-style flags. */
	dev->flags = IFF_NOARP;
	dev->features = NETIF_F_NO_CSUM;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
	dev->get_stats = can_get_stats;
#endif
}

/*
 * Function  alloc_candev
 * 	Allocates and sets up an CAN device
 */
struct net_device *alloc_candev(int sizeof_priv)
{
	struct net_device *dev;
	struct can_priv *priv;

	dev = alloc_netdev(sizeof_priv, "can%d", can_setup);
	if (!dev)
		return NULL;

	priv = netdev_priv(dev);

	priv->bitrate = CAN_BITRATE_UNCONFIGURED;
	priv->max_brp = DEFAULT_MAX_BRP;
	priv->max_sjw = DEFAULT_MAX_SJW;
	spin_lock_init(&priv->irq_lock);

	init_timer(&priv->timer);
	priv->timer.expires = 0;

	return dev;
}

EXPORT_SYMBOL(alloc_candev);

void free_candev(struct net_device *dev)
{
	free_netdev(dev);
}

EXPORT_SYMBOL(free_candev);

/*
 * CAN bus-off handling
 * FIXME: we need some synchronization
 */
int can_restart_now(struct net_device *dev)
{
	struct can_priv *priv = netdev_priv(dev);
	struct net_device_stats *stats = dev->get_stats(dev);
	struct sk_buff *skb;
	struct can_frame *cf;
	int err;

	/* Cancel restart in progress */
	if (priv->timer.expires) {
		del_timer(&priv->timer);
		priv->timer.expires = 0; /* mark inactive timer */
	}

	if ((err = priv->do_set_mode(dev, CAN_MODE_START)))
		return err;

	if (!netif_carrier_ok(dev))
		netif_carrier_on(dev);

	priv->can_stats.restarts++;

	/* send restart message upstream */
	skb = dev_alloc_skb(sizeof(struct can_frame));
	if (skb == NULL)
		return -ENOMEM;
	skb->dev = dev;
	skb->protocol = htons(ETH_P_CAN);
	cf = (struct can_frame *)skb_put(skb, sizeof(struct can_frame));
	memset(cf, 0, sizeof(struct can_frame));
	cf->can_id = CAN_ERR_FLAG | CAN_ERR_RESTARTED;
	cf->can_dlc = CAN_ERR_DLC;

	netif_rx(skb);

	dev->last_rx = jiffies;
	stats->rx_packets++;
	stats->rx_bytes += cf->can_dlc;

	return 0;
}

static void can_restart_after(unsigned long data)
{
	struct net_device *dev = (struct net_device *)data;
	struct can_priv *priv = netdev_priv(dev);

	priv->timer.expires = 0; /* mark inactive timer */
	can_restart_now(dev);
}

void can_bus_off(struct net_device *dev)
{
	struct can_priv *priv = netdev_priv(dev);

	netif_carrier_off(dev);

	if (priv->restart_ms > 0 && !priv->timer.expires) {

		priv->timer.function = can_restart_after;
		priv->timer.data = (unsigned long)dev;
		priv->timer.expires =
			jiffies + (priv->restart_ms * HZ) / 1000;
		add_timer(&priv->timer);
	}
}
EXPORT_SYMBOL(can_bus_off);

void can_close_cleanup(struct net_device *dev)
{
	struct can_priv *priv = netdev_priv(dev);

	if (priv->timer.expires) {
		del_timer(&priv->timer);
		priv->timer.expires = 0;
	}
}
EXPORT_SYMBOL(can_close_cleanup);

static __init int can_dev_init(void)
{
	return can_sysfs_init();
}
module_init(can_dev_init);

static __exit void can_dev_exit(void)
{
	can_sysfs_exit();
}
module_exit(can_dev_exit);
