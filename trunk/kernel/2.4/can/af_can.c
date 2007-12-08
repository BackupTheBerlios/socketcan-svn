/*
 * af_can.c - Protocol family CAN core module
 *            (used by different CAN protocol modules)
 *
 * Copyright (c) 2002-2007 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
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

#define EXPORT_SYMTAB

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/socket.h>
#include <linux/if_ether.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <net/sock.h>
#include <asm/uaccess.h>

#include "can.h"
#include "can_core.h"
#include "version.h"
#include "af_can.h"

RCSID("$Id$");

#define IDENT "af_can"
static __initdata const char banner[] = KERN_INFO "CAN: Controller Area "
					"Network PF_CAN core " VERSION "\n";

MODULE_DESCRIPTION("Controller Area Network PF_CAN core");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Urs Thuermann <urs.thuermann@volkswagen.de>, "
	      "Oliver Hartkopp <oliver.hartkopp@volkswagen.de>");

int stats_timer = 1; /* default: on */
MODULE_PARM(stats_timer, "1i");

struct notifier {
	struct list_head list;
	struct net_device *dev;
	void (*func)(unsigned long msg, void *data);
	void *data;
};

static LIST_HEAD(notifier_list);
static rwlock_t notifier_lock = RW_LOCK_UNLOCKED;

static struct dev_rcv_lists can_rx_alldev_list;
struct dev_rcv_lists *can_rx_dev_list;
rwlock_t can_rcvlists_lock = RW_LOCK_UNLOCKED;

static kmem_cache_t *rcv_cache;

/* table of registered CAN protocols */
static struct can_proto *proto_tab[CAN_NPROTO];

extern struct timer_list can_stattimer; /* timer for statistics update */
extern struct s_stats  can_stats;       /* packet statistics */
extern struct s_pstats can_pstats;      /* receive list statistics */

/*
 * af_can socket functions
 */

static int can_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	int err;
	struct sock *sk = sock->sk;

	switch (cmd) {
	case SIOCGSTAMP:
		if (sk->stamp.tv_sec == 0)
			return -ENOENT;
		if (err = copy_to_user((void *)arg, &sk->stamp,
				       sizeof(sk->stamp)))
			return err;
		break;
	default:
		return dev_ioctl(cmd, (void *)arg);
	}
	return 0;
}

static void can_sock_destruct(struct sock *sk)
{
	skb_queue_purge(&sk->receive_queue);
}

static int can_create(struct socket *sock, int protocol)
{
	struct sock *sk;
	struct can_proto *cp;
	int err;

	sock->state = SS_UNCONNECTED;

	if (protocol < 0 || protocol >= CAN_NPROTO)
		return -EINVAL;

	/* try to load protocol module, when CONFIG_KMOD is defined */
	if (!proto_tab[protocol]) {
		char module_name[30];
		sprintf(module_name, "can-proto-%d", protocol);
		if (request_module(module_name) == -ENOSYS)
			printk(KERN_INFO "CAN: request_module(%s) not"
			       " implemented.\n", module_name);
	}

	/* check for success and correct type */
	cp = proto_tab[protocol];
	if (!cp || cp->type != sock->type)
		return -EPROTONOSUPPORT;

	if (cp->capability >= 0 && !capable(cp->capability))
		return -EPERM;

	sock->ops = cp->ops;

	sk = sk_alloc(PF_CAN, GFP_KERNEL, 1);
	if (!sk)
		return -ENOMEM;

	sock_init_data(sock, sk);
	sk->destruct = can_sock_destruct;

	err = 0;
	if (cp->init)
		err = cp->init(sk);
	if (err) {
		/* release sk on errors */
		sock_orphan(sk);
		sock_put(sk);
		return err;
	}

	return 0;
}

/*
 * af_can tx path
 */

/**
 * can_send - transmit a CAN frame (optional with local loopback)
 * @skb: pointer to socket buffer with CAN frame in data section
 * @loop: loopback for listeners on local CAN sockets (recommended default!)
 *
 * Return:
 *  0 on success
 *  -ENETDOWN when the selected interface is down
 *  -ENOBUFS on full driver queue (see net_xmit_errno())
 */
int can_send(struct sk_buff *skb, int loop)
{
	int err;

	if (loop) {
		/* local loopback of sent CAN frames */

		/* indication for the CAN driver: do loopback */
		*(struct sock **)skb->cb = skb->sk;

		/* interface not capabable to do the loopback itself? */
		if (!(skb->dev->flags & IFF_LOOPBACK)) {
			struct sk_buff *newskb = skb_clone(skb, GFP_ATOMIC);
			newskb->protocol  = htons(ETH_P_CAN);
			newskb->ip_summed = CHECKSUM_UNNECESSARY;
			netif_rx(newskb);
		}
	} else {
		/* indication for the CAN driver: no loopback required */
		*(struct sock **)skb->cb = NULL;
	}

	if (!(skb->dev->flags & IFF_UP))
		return -ENETDOWN;

	/* send to netdevice */
	err = dev_queue_xmit(skb);
	if (err > 0)
		err = net_xmit_errno(err);

	/* update statistics */
	can_stats.tx_frames++;
	can_stats.tx_frames_delta++;

	return err;
}

/*
 * af_can rx path
 */

static struct dev_rcv_lists *find_dev_rcv_lists(struct net_device *dev)
{
	struct dev_rcv_lists *d;

	/* find receive list for this device */

	if (!dev)
		return &can_rx_alldev_list;

	for (d = can_rx_dev_list; d; d = d->next)
		if (d->dev == dev)
			break;

	return d;
}

static struct receiver **find_rcv_list(canid_t *can_id, canid_t *mask,
				       struct dev_rcv_lists *d)
{
	canid_t inv = *can_id & CAN_INV_FILTER; /* save flag before masking */

	/* filter error frames */
	if (*mask & CAN_ERR_FLAG) {
		/* clear CAN_ERR_FLAG in list entry */
		*mask &= CAN_ERR_MASK;
		return &d->rx_err;
	}

	/* ensure valid values in can_mask */
	if (*mask & CAN_EFF_FLAG)
		*mask &= (CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG);
	else
		*mask &= (CAN_SFF_MASK | CAN_RTR_FLAG);

	/* reduce condition testing at receive time */
	*can_id &= *mask;

	/* inverse can_id/can_mask filter */
	if (inv)
		return &d->rx_inv;

	/* mask == 0 => no condition testing at receive time */
	if (!(*mask))
		return &d->rx_all;

	/* use extra filterset for the subscription of exactly *ONE* can_id */
	if (*can_id & CAN_EFF_FLAG) {
		if (*mask == (CAN_EFF_MASK | CAN_EFF_FLAG))
			/* RFC: a use-case for hash-tables in the future? */
			return &d->rx_eff;
	} else {
		if (*mask == CAN_SFF_MASK)
			return &d->rx_sff[*can_id];
	}

	/* default: filter via can_id/can_mask */
	return &d->rx_fil;
}

/**
 * can_rx_register - subscribe CAN frames from a specific interface
 * @dev: pointer to netdevice (NULL => subcribe from 'all' CAN devices list)
 * @can_id: CAN identifier (see description)
 * @mask: CAN mask (see description)
 * @func: callback function on filter match
 * @data: returned parameter for callback function
 * @ident: string for calling module indentification
 *
 * Description:
 *  Invokes the callback function with the received sk_buff and the given
 *  parameter 'data' on a matching receive filter. A filter matches, when
 *
 *          <received_can_id> & mask == can_id & mask
 *
 *  The filter can be inverted (CAN_INV_FILTER bit set in can_id) or it can
 *  filter for error frames (CAN_ERR_FLAG bit set in mask).
 *
 * Return:
 *  0 on success
 *  -ENOMEM on missing cache mem to create subscription entry
 *  -ENODEV unknown device
 */
int can_rx_register(struct net_device *dev, canid_t can_id, canid_t mask,
		    void (*func)(struct sk_buff *, void *), void *data,
		    char *ident)
{
	struct receiver *r, **rl;
	struct dev_rcv_lists *d;
	int err = 0;

	/* insert new receiver  (dev,canid,mask) -> (func,data) */

	r = kmem_cache_alloc(rcv_cache, GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	write_lock_bh(&can_rcvlists_lock);

	d = find_dev_rcv_lists(dev);
	if (d) {
		rl = find_rcv_list(&can_id, &mask, d);

		r->can_id  = can_id;
		r->mask    = mask;
		r->matches = 0;
		r->func    = func;
		r->data    = data;
		r->ident   = ident;

		r->next = *rl;
		*rl = r;
		d->entries++;

		can_pstats.rcv_entries++;
		if (can_pstats.rcv_entries_max < can_pstats.rcv_entries)
			can_pstats.rcv_entries_max = can_pstats.rcv_entries;
	} else {
		kmem_cache_free(rcv_cache, r);
		err = -ENODEV;
	}
	write_unlock_bh(&can_rcvlists_lock);

	return err;
}

static void can_rx_delete_all(struct receiver **rl)
{
	struct receiver *r, *n;

	for (r = *rl; r; r = n) {
		n = r->next;
		kfree(r);
	}
	*rl = NULL;
}

/**
 * can_rx_unregister - unsubscribe CAN frames from a specific interface
 * @dev: pointer to netdevice (NULL => unsubcribe from 'all' CAN devices list)
 * @can_id: CAN identifier
 * @mask: CAN mask
 * @func: callback function on filter match
 * @data: returned parameter for callback function
 *
 * Description:
 *  Removes subscription entry depending on given (subscription) values.
 *
 * Return:
 *  0 on success
 *  -EINVAL on missing subscription entry
 *  -ENODEV unknown device
 */
int can_rx_unregister(struct net_device *dev, canid_t can_id, canid_t mask,
		      void (*func)(struct sk_buff *, void *), void *data)
{
	struct receiver *r, **rl;
	struct dev_rcv_lists *d;
	int err = 0;

	write_lock_bh(&can_rcvlists_lock);

	d = find_dev_rcv_lists(dev);
	if (!d) {
		err = -ENODEV;
		goto out;
	}

	rl = find_rcv_list(&can_id, &mask, d);

	/*
	 * Search the receiver list for the item to delete.  This should
	 * exist, since no receiver may be unregistered that hasn't
	 * been registered before.
	 */

	for (; r = *rl; rl = &r->next) {
		if (r->can_id == can_id && r->mask == mask
		    && r->func == func && r->data == data)
			break;
	}

	/*
	 * Check for bugs in CAN protocol implementations:
	 * If no matching list item was found, r is NULL.
	 */

	if (!r) {
		err = -EINVAL;
		goto out;
	}

	*rl = r->next;
	kmem_cache_free(rcv_cache, r);
	d->entries--;

	if (can_pstats.rcv_entries > 0)
		can_pstats.rcv_entries--;

 out:
	write_unlock_bh(&can_rcvlists_lock);

	return err;
}

static inline void deliver(struct sk_buff *skb, struct receiver *r)
{
	struct sk_buff *clone = skb_clone(skb, GFP_ATOMIC);
	if (clone) {
		r->func(clone, r->data);
		r->matches++;
	}
}

static int can_rcv_filter(struct dev_rcv_lists *d, struct sk_buff *skb)
{
	struct receiver *r;
	int matches = 0;
	struct can_frame *cf = (struct can_frame*)skb->data;
	canid_t can_id = cf->can_id;

	if (d->entries == 0)
		return 0;

	if (can_id & CAN_ERR_FLAG) {
		/* check for error frame entries only */
		for (r = d->rx_err; r; r = r->next) {
			if (can_id & r->mask) {
				deliver(skb, r);
				matches++;
			}
		}
		return matches;
	}

	/* check for unfiltered entries */
	for (r = d->rx_all; r; r = r->next) {
		deliver(skb, r);
		matches++;
	}

	/* check for can_id/mask entries */
	for (r = d->rx_fil; r; r = r->next) {
		if ((can_id & r->mask) == r->can_id) {
			deliver(skb, r);
			matches++;
		}
	}

	/* check for inverted can_id/mask entries */
	for (r = d->rx_inv; r; r = r->next) {
		if ((can_id & r->mask) != r->can_id) {
			deliver(skb, r);
			matches++;
		}
	}

	/* check CAN_ID specific entries */
	if (can_id & CAN_EFF_FLAG) {
		for (r = d->rx_eff; r; r = r->next) {
			if (r->can_id == can_id) {
				deliver(skb, r);
				matches++;
			}
		}
	} else {
		for (r = d->rx_sff[can_id & CAN_SFF_MASK]; r; r = r->next) {
			deliver(skb, r);
			matches++;
		}
	}

	return matches;
}

static int can_rcv(struct sk_buff *skb, struct net_device *dev,
		   struct packet_type *pt)
{
	struct dev_rcv_lists *d;
	int matches;

	/* update statistics */
	can_stats.rx_frames++;
	can_stats.rx_frames_delta++;

	read_lock(&can_rcvlists_lock);

	/* deliver the packet to sockets listening on all devices */
	matches = can_rcv_filter(&can_rx_alldev_list, skb);

	/* find receive list for this device */
	d = find_dev_rcv_lists(dev);
	if (d)
		matches += can_rcv_filter(d, skb);

	read_unlock(&can_rcvlists_lock);

	/* free the skbuff allocated by the netdevice driver */
	kfree_skb(skb);

	if (matches > 0) {
		can_stats.matches++;
		can_stats.matches_delta++;
	}

	return 0;
}


/*
 * af_can protocol functions
 */

/**
 * can_proto_register - register CAN transport protocol
 * @cp: pointer to CAN protocol structure
 */
void can_proto_register(struct can_proto *cp)
{
	int proto = cp->protocol;
	if (proto < 0 || proto >= CAN_NPROTO) {
		printk(KERN_ERR "CAN: protocol number %d out "
		       "of range\n", proto);
		return;
	}
	if (proto_tab[proto]) {
		printk(KERN_ERR "CAN: protocol %d already "
		       "registered\n", proto);
		return;
	}
	proto_tab[proto] = cp;

	/* use generic ioctl function if the module doesn't bring its own */
	if (!cp->ops->ioctl)
		cp->ops->ioctl = can_ioctl;
}

/**
 * can_proto_unregister - unregister CAN transport protocol
 * @cp: pointer to CAN protocol structure
 */
void can_proto_unregister(struct can_proto *cp)
{
	int proto = cp->protocol;
	if (!proto_tab[proto]) {
		printk(KERN_ERR "CAN: protocol %d is not registered\n", proto);
		return;
	}
	proto_tab[proto] = NULL;
}

/**
 * can_dev_register - subscribe notifier for CAN device status changes
 * @dev: pointer to netdevice
 * @func: callback function on status change
 * @data: returned parameter for callback function
 *
 * Description:
 *  Invokes the callback function with the status 'msg' and the given
 *  parameter 'data' on a status change of the given CAN network device.
 */
void can_dev_register(struct net_device *dev,
		      void (*func)(unsigned long msg, void *), void *data)
{
	struct notifier *n;

	n = kmalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		return;

	n->dev  = dev;
	n->func = func;
	n->data = data;

	write_lock(&notifier_lock);
	list_add(&n->list, &notifier_list);
	write_unlock(&notifier_lock);
}

/**
 * can_dev_unregister - unsubscribe notifier for CAN device status changes
 * @dev: pointer to netdevice
 * @func: callback function on filter match
 * @data: returned parameter for callback function
 *
 * Description:
 *  Removes subscription entry depending on given (subscription) values.
 */
void can_dev_unregister(struct net_device *dev,
			void (*func)(unsigned long msg, void *), void *data)
{
	struct notifier *n, *next;

	write_lock(&notifier_lock);
	list_for_each_entry_safe(n, next, &notifier_list, list) {
		if (n->dev == dev && n->func == func && n->data == data) {
			list_del(&n->list);
			kfree(n);
			break;
		}
	}
	write_unlock(&notifier_lock);
}

static int can_notifier(struct notifier_block *nb,
			unsigned long msg, void *data)
{
	struct net_device *dev = (struct net_device *)data;
	struct notifier *n;

	if (dev->type != ARPHRD_CAN)
		return NOTIFY_DONE;

	switch (msg) {
		struct dev_rcv_lists *d;
		int i;

	case NETDEV_REGISTER:

		/* create new dev_rcv_lists for this device */

		d = kmalloc(sizeof(*d),
			    in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if (!d) {
			printk(KERN_ERR "CAN: allocation of receive "
			       "list failed\n");
			return NOTIFY_DONE;
		}
		memset(d, 0, sizeof(*d));
		d->dev = dev;

		/* insert d into the list */
		write_lock_bh(&can_rcvlists_lock);
		d->next        = can_rx_dev_list;
		d->pprev       = &can_rx_dev_list;
		can_rx_dev_list    = d;
		if (d->next)
			d->next->pprev = &d->next;
		write_unlock_bh(&can_rcvlists_lock);

		break;

	case NETDEV_UNREGISTER:
		write_lock_bh(&can_rcvlists_lock);

		d = find_dev_rcv_lists(dev);
		if (!d) {
			printk(KERN_ERR "CAN: notifier: receive list not "
			       "found for dev %s\n", DNAME(dev));
			goto unreg_out;
		}

		/* remove d from the list */
		*d->pprev = d->next;
		d->next->pprev = d->pprev;

		/* remove all receivers hooked at this netdevice */
		can_rx_delete_all(&d->rx_err);
		can_rx_delete_all(&d->rx_all);
		can_rx_delete_all(&d->rx_fil);
		can_rx_delete_all(&d->rx_inv);
		can_rx_delete_all(&d->rx_eff);
		for (i = 0; i < 2048; i++)
			can_rx_delete_all(&d->rx_sff[i]);
		kfree(d);

	unreg_out:
		write_unlock_bh(&can_rcvlists_lock);

		break;
	}

	read_lock(&notifier_lock);
	list_for_each_entry(n, &notifier_list, list) {
		if (n->dev == dev)
			n->func(msg, n->data);
	}
	read_unlock(&notifier_lock);

	return NOTIFY_DONE;
}

/*
 * af_can module init/exit functions
 */

static struct packet_type can_packet = {
	.type = __constant_htons(ETH_P_CAN),
	.dev  = NULL,
	.func = can_rcv,
};

static struct net_proto_family can_family_ops = {
	.family = PF_CAN,
	.create = can_create,
};

/* notifier block for netdevice event */
static struct notifier_block can_netdev_notifier = {
	.notifier_call = can_notifier,
};

static __init int can_init(void)
{
	struct net_device *dev;

	printk(banner);

	rcv_cache = kmem_cache_create("can_receiver", sizeof(struct receiver),
				      0, 0, NULL, NULL);
	if (!rcv_cache)
		return -ENOMEM;

	/*
	 * Insert struct dev_rcv_lists for reception on all devices.
	 * This struct is zero initialized which is correct for the
	 * embedded receiver list head pointer, the dev pointer,
	 * and the entries counter.
	 */

	write_lock_bh(&can_rcvlists_lock);
	can_rx_alldev_list.pprev = &can_rx_dev_list;
	can_rx_dev_list          = &can_rx_alldev_list;
	write_unlock_bh(&can_rcvlists_lock);

	if (stats_timer) {
		/* statistics init */
		init_timer(&can_stattimer);
	}

	can_init_proc();

	/* protocol register */
	sock_register(&can_family_ops);

	/* netdevice notifier register & init currently existing devices */
	read_lock_bh(&dev_base_lock);
	register_netdevice_notifier(&can_netdev_notifier);
	for (dev = dev_base; dev; dev = dev->next)
		can_netdev_notifier.notifier_call(&can_netdev_notifier,
						  NETDEV_REGISTER,
						  dev);
	read_unlock_bh(&dev_base_lock);

	dev_add_pack(&can_packet);

	return 0;
}

static __exit void can_exit(void)
{
	struct dev_rcv_lists *d;

	if (stats_timer)
		del_timer(&can_stattimer);

	can_remove_proc();

	/* protocol unregister */
	dev_remove_pack(&can_packet);
	unregister_netdevice_notifier(&can_netdev_notifier);
	sock_unregister(PF_CAN);

	/* remove can_rx_dev_list */
	write_lock_bh(&can_rcvlists_lock);
	for (d = can_rx_dev_list; d; d = d->next)
		if (d != &can_rx_alldev_list)
			kfree(d);
	can_rx_dev_list = NULL;
	write_unlock_bh(&can_rcvlists_lock);

	kmem_cache_destroy(rcv_cache);
}

/*
 * af_can utility stuff
 */

unsigned long timeval2jiffies(struct timeval *tv, int round_up)
{
	unsigned long jif;
	unsigned long sec  = tv->tv_sec;
	unsigned long usec = tv->tv_usec;

	if (sec > ULONG_MAX / HZ)          /* check for overflow */
		return ULONG_MAX;

	if (round_up)                      /* any usec below one HZ? */
		usec += 1000000 / HZ - 1;  /* pump it up */

	jif = usec / (1000000 / HZ);

	if (sec * HZ > ULONG_MAX - jif)    /* check for overflow */
		return ULONG_MAX;
	else
		return jif + sec * HZ;
}



module_init(can_init);
module_exit(can_exit);

/*
 * Exported symbols
 */
#ifdef EXPORT_SYMTAB
EXPORT_SYMBOL(can_proto_register);
EXPORT_SYMBOL(can_proto_unregister);
EXPORT_SYMBOL(can_rx_register);
EXPORT_SYMBOL(can_rx_unregister);
EXPORT_SYMBOL(can_dev_register);
EXPORT_SYMBOL(can_dev_unregister);
EXPORT_SYMBOL(can_send);
EXPORT_SYMBOL(timeval2jiffies);
#endif
