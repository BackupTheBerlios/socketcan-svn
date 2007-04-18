/*
 * vcan.c - Virtual CAN interface
 *
 * Copyright (c) 2002-2007 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, the following disclaimer and
 *    the referenced file 'COPYING'.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2 as distributed in the 'COPYING'
 * file from the main directory of the linux kernel source.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <socketcan-users@lists.berlios.de>
 *
 */

#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>

#include <linux/can.h>
#include <linux/can/version.h>

RCSID("$Id$");

static __initdata const char banner[] = KERN_INFO "CAN: virtual CAN "
					"interface " VERSION "\n"; 

MODULE_DESCRIPTION("virtual CAN interface");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Urs Thuermann <urs.thuermann@volkswagen.de>");

#ifdef CONFIG_CAN_DEBUG_DEVICES
static int debug = 0;
module_param(debug, int, S_IRUGO);
#define DBG(args...)       (debug & 1 ? \
			       (printk(KERN_DEBUG "VCAN %s: ", __func__), \
				printk(args)) : 0)
#define DBG_FRAME(args...) (debug & 2 ? can_debug_cframe(args) : 0)
#define DBG_SKB(skb)       (debug & 4 ? can_debug_skb(skb) : 0)
#else
#define DBG(args...)
#define DBG_FRAME(args...)
#define DBG_SKB(skb)
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,14)
static void *kzalloc(size_t size, unsigned int __nocast flags)
{
	void *ret = kmalloc(size, flags);
	if (ret)
		memset(ret, 0, size);
	return ret;
}
#endif

/* Indicate if this VCAN driver should do a real loopback, or if this */
/* should be done in af_can.c */
#undef  DO_LOOPBACK

#define STATSIZE sizeof(struct net_device_stats)

static int numdev = 4; /* default number of virtual CAN interfaces */
module_param(numdev, int, S_IRUGO);
MODULE_PARM_DESC(numdev, "Number of virtual CAN devices");

static struct net_device **vcan_devs; /* root pointer to netdevice structs */

static int vcan_open(struct net_device *dev)
{
	DBG("%s: interface up\n", dev->name);

	netif_start_queue(dev);
	return 0;
}

static int vcan_stop(struct net_device *dev)
{
	DBG("%s: interface down\n", dev->name);

	netif_stop_queue(dev);
	return 0;
}

#ifdef DO_LOOPBACK

static void vcan_rx(struct sk_buff *skb, struct net_device *dev)
{
	struct net_device_stats *stats = netdev_priv(dev);
	stats->rx_packets++;
	stats->rx_bytes += skb->len;

	skb->protocol  = htons(ETH_P_CAN);
	skb->dev       = dev;
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	DBG("received skbuff on interface %d\n", dev->ifindex);
	DBG_SKB(skb);

	netif_rx(skb);
}

#endif

static int vcan_tx(struct sk_buff *skb, struct net_device *dev)
{
	struct net_device_stats *stats = netdev_priv(dev);
	int loop;

	DBG("sending skbuff on interface %s\n", dev->name);
	DBG_SKB(skb);
	DBG_FRAME("VCAN: transmit CAN frame", (struct can_frame *)skb->data);

	stats->tx_packets++;
	stats->tx_bytes += skb->len;

	/* loopback (on driver level) required? */
	loop = *(struct sock **)skb->cb != NULL;

#ifdef DO_LOOPBACK
	if (loop) {
		if (atomic_read(&skb->users) != 1) {
			struct sk_buff *old_skb = skb;

			skb = skb_clone(old_skb, GFP_ATOMIC);
			DBG("  freeing old skbuff %p, using new skbuff %p\n",
			    old_skb, skb);
			kfree_skb(old_skb);
			if (!skb) {
				return 0;
			}
		} else
			skb_orphan(skb);

		/* receive with packet counting */
		vcan_rx(skb, dev);
	} else {
		/* no looped packets => no counting */
		kfree_skb(skb);
	}
#else
	/* only count here, because the CAN core already did the loopback */
	if (loop) {
		stats->rx_packets++;
		stats->rx_bytes += skb->len;
	}
	kfree_skb(skb);
#endif
	return 0;
}

static int vcan_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	return -EOPNOTSUPP;
}

static int vcan_rebuild_header(struct sk_buff *skb)
{
	DBG("skbuff %p\n", skb);
	DBG_SKB(skb);
	return 0;
}

static int vcan_header(struct sk_buff *skb, struct net_device *dev,
		       unsigned short type, void *daddr, void *saddr,
		       unsigned int len)
{
	DBG("skbuff %p, device %p\n", skb, dev);
	DBG_SKB(skb);
	return 0;
}


static struct net_device_stats *vcan_get_stats(struct net_device *dev)
{
	struct net_device_stats *stats = netdev_priv(dev);
	return stats;
}

static void vcan_init(struct net_device *dev)
{
	DBG("dev %s\n", dev->name);

	ether_setup(dev);

	memset(dev->priv, 0, STATSIZE);

	dev->type              = ARPHRD_CAN;
	dev->mtu               = sizeof(struct can_frame);
	dev->flags             = IFF_NOARP;
#ifdef DO_LOOPBACK
	dev->flags            |= IFF_LOOPBACK;
#endif

	dev->open              = vcan_open;
	dev->stop              = vcan_stop;
	dev->set_config        = NULL;
	dev->hard_start_xmit   = vcan_tx;
	dev->do_ioctl          = vcan_ioctl;
	dev->get_stats         = vcan_get_stats;
	dev->hard_header       = vcan_header;
	dev->rebuild_header    = vcan_rebuild_header;
	dev->hard_header_cache = NULL;

	SET_MODULE_OWNER(dev);
}

static __init int vcan_init_module(void)
{
	int i;
	int ndev = 0;
	int result = 0;

	printk(banner);

	/* register at least one interface */
	if (numdev < 1)
		numdev = 1;

	printk(KERN_INFO "vcan: registering %d virtual CAN interfaces.\n",
	       numdev );

	vcan_devs = kzalloc(numdev * sizeof(struct net_device *), GFP_KERNEL);
	if (!vcan_devs) {
		printk(KERN_ERR "vcan: Can't allocate vcan devices array!\n");
		return -ENOMEM;
	}

	for (i = 0; i < numdev; i++) {
		vcan_devs[i] = alloc_netdev(STATSIZE, "vcan%d", vcan_init);
		if (!vcan_devs[i]) {
			printk(KERN_ERR "vcan: error allocating net_device\n");
			result = -ENOMEM;
			goto out;
		}

		result = register_netdev(vcan_devs[i]);
		if (result < 0) {
			printk(KERN_ERR "vcan: error %d registering "
			       "interface %s\n",
			       result, vcan_devs[i]->name);
			free_netdev(vcan_devs[i]);
			vcan_devs[i] = NULL;
			goto out;

		} else {
			DBG("successfully registered interface %s\n",
			    vcan_devs[i]->name);
			ndev++;
		}
	}

	if (ndev)
		return 0;

 out:
	for (i = 0; i < numdev; i++) {
		if (vcan_devs[i]) {
			unregister_netdev(vcan_devs[i]);
			free_netdev(vcan_devs[i]);
		}
	}

	kfree(vcan_devs);

	return result;
}

static __exit void vcan_cleanup_module(void)
{
	int i;

	if (!vcan_devs)
		return;

	for (i = 0; i < numdev; i++) {
		if (vcan_devs[i]) {
			unregister_netdev(vcan_devs[i]);
			free_netdev(vcan_devs[i]);
		}
	}

	kfree(vcan_devs);
}

module_init(vcan_init_module);
module_exit(vcan_cleanup_module);
