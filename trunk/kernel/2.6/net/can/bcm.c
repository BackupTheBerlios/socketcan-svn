/*
 * bcm.c - Broadcast Manager to filter/send (cyclic) CAN content
 *
 * Copyright (c) 2002-2005 Volkswagen Group Electronic Research
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
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include <net/sock.h>

#include <linux/can.h>
#include <linux/can/bcm.h>
#include <linux/can/version.h>

#include "af_can.h"

RCSID("$Id$");

#ifdef CONFIG_CAN_DEBUG_CORE
static int debug = 0;
module_param(debug, int, S_IRUGO);
#define DBG(args...)       (debug & 1 ? \
			       (printk(KERN_DEBUG "BCM %s: ", __func__), \
				printk(args)) : 0)
#define DBG_FRAME(args...) (debug & 2 ? can_debug_cframe(args) : 0)
#define DBG_SKB(skb)       (debug & 4 ? can_debug_skb(skb) : 0)
#else
#define DBG(args...)
#define DBG_FRAME(args...)
#define DBG_SKB(skb)
#endif

/* use of last_frames[index].can_dlc */
#define RX_RECV    0x40 /* received data for this element */
#define RX_THR     0x80 /* this element has not been sent due to throttle functionality */
#define BCM_CAN_DLC_MASK 0x0F /* clean flags by masking with BCM_CAN_DLC_MASK */
#define BCM_RX_REGMASK (CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG)

#define NAME "Broadcast Manager (BCM) for LLCF"
#define IDENT "bcm"
static __initdata const char banner[] = BANNER(NAME);

MODULE_DESCRIPTION(NAME);
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Oliver Hartkopp <oliver.hartkopp@volkswagen.de>");

#define GET_U64(p) (*(unsigned long long*)(p)->data) /* easy access */

struct bcm_op {
	struct list_head list;
	int ifindex;
	canid_t can_id;
	int flags;
	unsigned long j_ival1, j_ival2, j_lastmsg;
	unsigned long frames_abs, frames_filtered;
	struct timer_list timer, thrtimer;
	struct timeval ival1, ival2;
	struct timeval rx_stamp;
	int rx_ifindex;
	int count;
	int nframes;
	int currframe;
	struct can_frame *frames;
	struct can_frame *last_frames;
	struct sock *sk;
};

struct bcm_opt {
	int bound;
	int ifindex;
	struct list_head rx_ops;
	struct list_head tx_ops;
	unsigned long dropped_usr_msgs;
	struct proc_dir_entry *bcm_proc_read;
	char procname [9]; /* pointer printed in ASCII with \0 */
};

static struct proc_dir_entry *proc_dir = NULL;

static int  bcm_init(struct sock *sk);
static void bcm_notifier(unsigned long msg, void *data);
static int  bcm_release(struct socket *sock);
static int  bcm_connect(struct socket *sock, struct sockaddr *uaddr, int len,
			int flags);
static int  bcm_sendmsg(struct kiocb *iocb, struct socket *sock,
			struct msghdr *msg, size_t size);
static int  bcm_recvmsg(struct kiocb *iocb, struct socket *sock,
			struct msghdr *msg, size_t size, int flags);
static unsigned int bcm_poll(struct file *file, struct socket *sock,
			     poll_table *wait);

static int  bcm_read_proc(char *page, char **start, off_t off,
			  int count, int *eof, void *data);

static void bcm_tx_timeout_handler(unsigned long data);
static int  bcm_tx_send(struct msghdr *msg, int ifindex, struct sock *sk);
static int  bcm_tx_setup(struct bcm_msg_head *msg_head, struct msghdr *msg,
			 int ifindex, struct sock *sk);
static void bcm_can_tx(struct bcm_op *op);

static int  bcm_rx_setup(struct bcm_msg_head *msg_head, struct msghdr *msg,
			 int ifindex, struct sock *sk);
static void bcm_rx_handler(struct sk_buff *skb, void *op);
static void bcm_rx_timeout_handler(unsigned long data);
static void bcm_rx_thr_handler(unsigned long data);
static void bcm_rx_cmp_to_index(struct bcm_op *op, int index,
				struct can_frame *rxdata);
static void bcm_rx_changed(struct bcm_op *op, struct can_frame *data);
static void bcm_rx_starttimer(struct bcm_op *op);
static void bcm_rx_update_and_send(struct bcm_op *op,
				   struct can_frame *lastdata,
				   struct can_frame *rxdata);
static void bcm_send_to_user(struct bcm_op *op, struct bcm_msg_head *head,
			     struct can_frame *frames, struct timeval *tv);

static int  bcm_delete_tx_op(struct list_head *ops, canid_t can_id, int ifindex);
static int  bcm_delete_rx_op(struct list_head *ops, canid_t can_id, int ifindex);
static void bcm_remove_op(struct bcm_op *op);
static int  bcm_read_op(struct list_head *ops, struct bcm_msg_head *msg_head, int ifindex);
static struct bcm_op *bcm_find_op(struct list_head *ops, canid_t can_id, int ifindex);

static struct proto_ops bcm_ops = {
	.family        = PF_CAN,
	.release       = bcm_release,
	.bind          = sock_no_bind,
	.connect       = bcm_connect,
	.socketpair    = sock_no_socketpair,
	.accept        = sock_no_accept,
	.getname       = sock_no_getname,
	.poll          = bcm_poll,
	.ioctl         = NULL,		/* use can_ioctl() from af_can.c */
	.listen        = sock_no_listen,
	.shutdown      = sock_no_shutdown,
	.setsockopt    = sock_no_setsockopt,
	.getsockopt    = sock_no_getsockopt,
	.sendmsg       = bcm_sendmsg,
	.recvmsg       = bcm_recvmsg,
	.mmap          = sock_no_mmap,
	.sendpage      = sock_no_sendpage,
};

#ifdef CONFIG_CAN_BCM_USER
#define BCM_CAP (-1)
#else
#define BCM_CAP CAP_NET_RAW
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,13)

struct bcm_sock {
	struct sock    sk;
	struct bcm_opt opt;
};

#define bcm_sk(sk) (&((struct bcm_sock *)(sk))->opt)

static struct proto bcm_proto = {
	.name       = "CAN_BCM",
	.owner      = THIS_MODULE,
	.obj_size   = sizeof(struct bcm_sock),
	.init       = bcm_init,
};

static struct can_proto bcm_can_proto = {
	.type       = SOCK_DGRAM,
	.protocol   = CAN_BCM,
	.capability = BCM_CAP,
	.ops        = &bcm_ops,
	.prot       = &bcm_proto,
};

#else

#define bcm_sk(sk) ((struct bcm_opt *)(sk)->sk_protinfo)

static struct can_proto bcm_can_proto = {
	.type       = SOCK_DGRAM,
	.protocol   = CAN_BCM,
	.capability = BCM_CAP,
	.ops        = &bcm_ops,
	.owner      = THIS_MODULE,
	.obj_size   = sizeof(struct bcm_opt),
	.init       = bcm_init,
};

#endif

#define CFSIZ sizeof(struct can_frame)
#define OPSIZ sizeof(struct bcm_op)
#define MHSIZ sizeof(struct bcm_msg_head)

static int __init bcm_module_init(void)
{
	printk(banner);

	can_proto_register(&bcm_can_proto);

	/* create /proc/net/can/bcm directory */
	proc_dir = proc_mkdir(CAN_PROC_DIR"/"IDENT, NULL);

	if (proc_dir)
		proc_dir->owner = THIS_MODULE;

	return 0;
}

static void __exit bcm_module_exit(void)
{
	can_proto_unregister(&bcm_can_proto);

	if (proc_dir)
		remove_proc_entry(CAN_PROC_DIR"/"IDENT, NULL);

}

/**************************************************/
/* initial settings at socket creation time       */
/**************************************************/

static int bcm_init(struct sock *sk)
{
	struct bcm_opt *bo = bcm_sk(sk);

	bo->bound            = 0;
	bo->ifindex          = 0;
	bo->dropped_usr_msgs = 0;
	bo->bcm_proc_read    = NULL;

	INIT_LIST_HEAD(&bo->tx_ops);
	INIT_LIST_HEAD(&bo->rx_ops);

	return 0;
}

/**************************************************/
/* handling of netdevice problems                 */
/**************************************************/

static void bcm_notifier(unsigned long msg, void *data)
{
	struct sock *sk = (struct sock *)data;
	struct bcm_opt *bo = bcm_sk(sk);

	DBG("called for sock %p\n", sk);

	switch (msg) {
	case NETDEV_UNREGISTER:
		bo->bound   = 0;
		bo->ifindex = 0;
		/* fallthrough */
	case NETDEV_DOWN:
		sk->sk_err = ENETDOWN;
		if (!sock_flag(sk, SOCK_DEAD))
			sk->sk_error_report(sk);
	}
}

/**************************************************/
/* standard socket functions                      */
/**************************************************/

static int bcm_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct bcm_opt *bo = bcm_sk(sk);
	struct bcm_op *op, *n;

	DBG("socket %p, sk %p\n", sock, sk);

	/* remove bcm_ops, timer, rx_unregister(), etc. */

	list_for_each_entry_safe(op, n, &bo->tx_ops, list) {
		DBG("removing tx_op (%p) for can_id <%03X>\n", op, op->can_id);
		bcm_remove_op(op);
	}

	list_for_each_entry_safe(op, n, &bo->rx_ops, list) {
		DBG("removing rx_op (%p) for can_id <%03X>\n", op, op->can_id);

		/* Don't care if we're bound or not (due to netdev problems) */
		/* can_rx_unregister() is always a save thing to do here     */
		if (op->ifindex) {
			struct net_device *dev = dev_get_by_index(op->ifindex);
			if (dev) {
				can_rx_unregister(dev, op->can_id, BCM_RX_REGMASK, bcm_rx_handler, op);
				dev_put(dev);
			}
		} else
			can_rx_unregister(NULL, op->can_id, BCM_RX_REGMASK, bcm_rx_handler, op);

		bcm_remove_op(op);
	}


	/* remove procfs entry */
	if ((proc_dir) && (bo->bcm_proc_read)) {
		remove_proc_entry(bo->procname, proc_dir);
	}

	/* remove device notifier */
	if (bo->ifindex) {
		struct net_device *dev = dev_get_by_index(bo->ifindex);
		if (dev) {
			can_dev_unregister(dev, bcm_notifier, sk);
			dev_put(dev);
		}
	}

	sock_put(sk);

	return 0;
}

static int bcm_connect(struct socket *sock, struct sockaddr *uaddr, int len,
		       int flags)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct sock *sk = sock->sk;
	struct bcm_opt *bo = bcm_sk(sk);

	if (bo->bound)
		return -EISCONN;

	/* bind a device to this socket */
	if (addr->can_ifindex) {
		struct net_device *dev = dev_get_by_index(addr->can_ifindex);
		if (!dev) {
			DBG("could not find device index %d\n", addr->can_ifindex);
			return -ENODEV;
		}
		bo->ifindex = dev->ifindex;
		can_dev_register(dev, bcm_notifier, sk); /* register notifier */
		dev_put(dev);

		DBG("socket %p bound to device %s (idx %d)\n", sock, dev->name, dev->ifindex);
	} else
		bo->ifindex = 0; /* no notifier for ifindex = 0 ('any' CAN device) */

	bo->bound = 1;

	if (proc_dir) {
		sprintf(bo->procname, "%p", sock); /* unique socket address as filename */
		bo->bcm_proc_read = create_proc_read_entry(bo->procname, 0644,
							   proc_dir, bcm_read_proc, sk);
	}

	return 0;
}

static int bcm_sendmsg(struct kiocb *iocb, struct socket *sock,
		       struct msghdr *msg, size_t size)
{
	struct sock *sk = sock->sk;
	struct bcm_opt *bo = bcm_sk(sk);
	int ifindex = bo->ifindex; /* default ifindex for this bcm_op */
	struct bcm_msg_head msg_head;
	int ret; /* read bytes or error codes as return value */

	if (!bo->bound) {
		DBG("sock %p not bound\n", sk);
		return -ENOTCONN;
	}

	/* check for alternative ifindex for this bcm_op */

	if (!ifindex && msg->msg_name) { /* no bound device as default */
		struct sockaddr_can *addr = (struct sockaddr_can *)msg->msg_name;
		if (addr->can_family != AF_CAN)
			return -EINVAL;
		ifindex = addr->can_ifindex; /* ifindex from sendto() */

		if (ifindex && !dev_get_by_index(ifindex)) {
			DBG("device %d not found\n", ifindex);
			return -ENODEV;
		}
	}

	/* read message head information */

	if ((ret = memcpy_fromiovec((unsigned char*)&msg_head, msg->msg_iov,
				    MHSIZ)) < 0)
		return ret;

	DBG("opcode %d for can_id <%03X>\n", msg_head.opcode, msg_head.can_id);

	switch (msg_head.opcode) {

	case TX_SETUP:

		ret = bcm_tx_setup(&msg_head, msg, ifindex, sk);
		break;

	case RX_SETUP:

		ret = bcm_rx_setup(&msg_head, msg, ifindex, sk);
		break;

	case TX_DELETE:

		if (bcm_delete_tx_op(&bo->tx_ops, msg_head.can_id, ifindex))
			ret = MHSIZ;
		else
			ret = -EINVAL;
		break;
		    
	case RX_DELETE:

		if (bcm_delete_rx_op(&bo->rx_ops, msg_head.can_id, ifindex))
			ret = MHSIZ;
		else
			ret = -EINVAL;
		break;

	case TX_READ:

		/* reuse msg_head for the reply */
		msg_head.opcode  = TX_STATUS; /* reply to TX_READ */
		ret = bcm_read_op(&bo->tx_ops, &msg_head, ifindex);
		break;

	case RX_READ:

		/* reuse msg_head for the reply */
		msg_head.opcode  = RX_STATUS; /* reply to RX_READ */
		ret = bcm_read_op(&bo->rx_ops, &msg_head, ifindex);
		break;

	case TX_SEND:

		if (msg_head.nframes < 1) /* we need at least one can_frame */
			return -EINVAL;

		ret = bcm_tx_send(msg, ifindex, sk);
		break;

	default:

		DBG("Unknown opcode %d\n", msg_head.opcode);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int bcm_recvmsg(struct kiocb *iocb, struct socket *sock,
		       struct msghdr *msg, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int error = 0;
	int noblock;
	int err;

	DBG("socket %p, sk %p\n", sock, sk);

	noblock =  flags & MSG_DONTWAIT;
	flags   &= ~MSG_DONTWAIT;
	if (!(skb = skb_recv_datagram(sk, flags, noblock, &error))) {
		return error;
	}

	DBG("delivering skbuff %p\n", skb);
	DBG_SKB(skb);

	if (skb->len < size)
		size = skb->len;
	if ((err = memcpy_toiovec(msg->msg_iov, skb->data, size)) < 0) {
		skb_free_datagram(sk, skb);
		return err;
	}

	sock_recv_timestamp(msg, sk, skb);

	if (msg->msg_name) {
		msg->msg_namelen = sizeof(struct sockaddr_can);
		memcpy(msg->msg_name, skb->cb, msg->msg_namelen);
	}

	DBG("freeing sock %p, skbuff %p\n", sk, skb);
	skb_free_datagram(sk, skb);

	return size;
}

static unsigned int bcm_poll(struct file *file, struct socket *sock,
			     poll_table *wait)
{
	unsigned int mask = 0;

	DBG("socket %p\n", sock);

	mask = datagram_poll(file, sock, wait);
	return mask;
}

/**************************************************/
/* helper functions for bcm_sendmsg()             */
/**************************************************/

static int bcm_tx_setup(struct bcm_msg_head *msg_head, struct msghdr *msg,
			int ifindex, struct sock *sk)
{
	struct bcm_opt *bo = bcm_sk(sk);
	struct bcm_op *op;
	int i, err;

	if (!ifindex) /* we need a real device to send frames */
		return -ENODEV;

	if (msg_head->nframes < 1) /* we need at least one can_frame */
		return -EINVAL;

	/* check the given can_id */

	if ((op = bcm_find_op(&bo->tx_ops, msg_head->can_id, ifindex))) {

		/* update existing BCM operation */

		DBG("TX_SETUP: modifying existing tx_op (%p) for can_id <%03X>\n",
		    op, msg_head->can_id);

		/* Do we need more space for the can_frames than currently */
		/* allocated? -> This is a _really_ unusual use-case and   */
		/* therefore (complexity / locking) it is not supported.   */
		if (msg_head->nframes > op->nframes)
			return -E2BIG;

		/* update can_frames content */
		for (i = 0; i < msg_head->nframes; i++) {
			if ((err = memcpy_fromiovec((unsigned char*)&op->frames[i], msg->msg_iov, CFSIZ)) < 0)
				return err;

			if (msg_head->flags & TX_CP_CAN_ID)
				op->frames[i].can_id = msg_head->can_id; /* copy can_id into frame */
		}

	} else {
		/* insert new BCM operation for the given can_id */

		if (!(op = kmalloc(OPSIZ, GFP_KERNEL)))
			return -ENOMEM;

		memset(op, 0, OPSIZ); /* init to zero, e.g. for timers */

		DBG("TX_SETUP: creating new tx_op (%p) for can_id <%03X>\n",
		    op, msg_head->can_id);

		op->can_id    = msg_head->can_id;

		/* create array for can_frames and copy the data */
		if (!(op->frames = kmalloc(msg_head->nframes * CFSIZ, GFP_KERNEL))) {
			kfree(op);
			return -ENOMEM;
		}

		for (i = 0; i < msg_head->nframes; i++) {
			if ((err = memcpy_fromiovec((unsigned char*)&op->frames[i], msg->msg_iov, CFSIZ)) < 0) {
				kfree(op->frames);
				kfree(op);
				return err;
			}

			if (msg_head->flags & TX_CP_CAN_ID)
				op->frames[i].can_id = msg_head->can_id; /* copy can_id into frame */
		}

		op->last_frames = NULL; /* tx_ops never compare with previous received messages */

		op->sk = sk; /* bcm_can_tx / bcm_tx_timeout_handler needs this */
		op->ifindex = ifindex;

		init_timer(&op->timer); /* initialize uninitialized (kmalloc) structure */
		init_timer(&op->thrtimer); /* currently unused in tx_ops */

		op->timer.function = bcm_tx_timeout_handler; /* handler for tx_ops */
		op->timer.data = (unsigned long)op; /* timer.data points to this op-structure */

		/* add this bcm_op to the list of the tx_ops */
		list_add(&op->list, &bo->tx_ops);

	} /* if ((op = bcm_find_op(&bo->tx_ops, msg_head->can_id, ifindex))) */

	if (op->nframes != msg_head->nframes) {
		op->nframes   = msg_head->nframes;
		op->currframe = 0; /* start multiple frame transmission with index 0 */
	}

	/* check flags */

	op->flags = msg_head->flags;

	if (op->flags & TX_RESET_MULTI_IDX)
		op->currframe = 0; /* start multiple frame transmission with index 0 */

	if (op->flags & SETTIMER) {

		/* set timer values */

		op->count   = msg_head->count;
		op->ival1   = msg_head->ival1;
		op->ival2   = msg_head->ival2;
		op->j_ival1 = timeval2jiffies(&msg_head->ival1, 1);
		op->j_ival2 = timeval2jiffies(&msg_head->ival2, 1);

		DBG("TX_SETUP: SETTIMER count=%d j_ival1=%ld j_ival2=%ld\n",
		    op->count, op->j_ival1, op->j_ival2);

		/* disable an active timer due to zero values? */
		if (!op->j_ival1 && !op->j_ival2) {
			del_timer(&op->timer);
			DBG("TX_SETUP: SETTIMER disabled timer.\n");
		}
	}

	if ((op->flags & STARTTIMER) && ((op->j_ival1 && op->count) || op->j_ival2)) {

		del_timer(&op->timer);

		op->flags |= TX_ANNOUNCE; /* spec: send can_frame when starting timer */
		if (op->j_ival1 && (op->count > 0)){
			op->timer.expires = jiffies + op->j_ival1;
			/* op->count-- is done in bcm_tx_timeout_handler */
			DBG("TX_SETUP: adding timer ival1. func=%p data=(%p) exp=0x%08X\n",
			    op->timer.function,
			    (char*) op->timer.data,
			    (unsigned int) op->timer.expires);
		} else{
			op->timer.expires = jiffies + op->j_ival2;
			DBG("TX_SETUP: adding timer ival2. func=%p data=(%p) exp=0x%08X\n",
			    op->timer.function,
			    (char*) op->timer.data,
			    (unsigned int) op->timer.expires);
		}

		add_timer(&op->timer);
	}

	if (op->flags & TX_ANNOUNCE)
		bcm_can_tx(op);

	return msg_head->nframes * CFSIZ + MHSIZ;
}

static int bcm_rx_setup(struct bcm_msg_head *msg_head, struct msghdr *msg,
			int ifindex, struct sock *sk)
{
	struct bcm_opt *bo = bcm_sk(sk);
	struct bcm_op *op;
	int do_rx_register;
	int err;

	if ((msg_head->flags & RX_FILTER_ID) || (!(msg_head->nframes))) {
		/* be robust against wrong usage ... */
		msg_head->flags |= RX_FILTER_ID;
		msg_head->nframes = 0; /* ignore trailing garbage */
	}

	if ((msg_head->flags & RX_RTR_FRAME) &&
	    ((msg_head->nframes != 1) || (!(msg_head->can_id & CAN_RTR_FLAG)))) {

		DBG("RX_SETUP: bad RX_RTR_FRAME setup!\n");
		return -EINVAL;
	}

	/* check the given can_id */

	if ((op = bcm_find_op(&bo->rx_ops, msg_head->can_id, ifindex))) {

		/* update existing BCM operation */

		DBG("RX_SETUP: modifying existing rx_op (%p) for can_id <%03X>\n",
		    op, msg_head->can_id);

		/* Do we need more space for the can_frames than currently */
		/* allocated? -> This is a _really_ unusual use-case and   */
		/* therefore (complexity / locking) it is not supported.   */
		if (msg_head->nframes > op->nframes)
			return -E2BIG;

		if (msg_head->nframes) {
			/* update can_frames content */
			if ((err = memcpy_fromiovec((unsigned char*)op->frames, msg->msg_iov, msg_head->nframes * CFSIZ) < 0))
				return err;

			/* clear received can_frames to indicate 'nothing received' */
			memset(op->last_frames, 0, msg_head->nframes * CFSIZ);
		}

		op->nframes = msg_head->nframes;
		/* Only an update -> do not call can_rx_register() at end of RX_SETUP */
		do_rx_register = 0;

	} else {
		/* insert new BCM operation for the given can_id */

		if (!(op = kmalloc(OPSIZ, GFP_KERNEL)))
			return -ENOMEM;

		memset(op, 0, OPSIZ); /* init to zero, e.g. for timers */

		DBG("RX_SETUP: creating new rx_op (%p) for can_id <%03X>\n",
		    op, msg_head->can_id);

		op->can_id    = msg_head->can_id;
		op->nframes   = msg_head->nframes;

		if (msg_head->nframes) {

			/* create array for can_frames and copy the data */
			if (!(op->frames = kmalloc(msg_head->nframes * CFSIZ, GFP_KERNEL))) {
				kfree(op);
				return -ENOMEM;
			}

			if ((err = memcpy_fromiovec((unsigned char*)op->frames, msg->msg_iov, msg_head->nframes * CFSIZ)) < 0) {
				kfree(op->frames);
				kfree(op);
				return err;
			}

			/* create array for received can_frames */
			if (!(op->last_frames = kmalloc(msg_head->nframes * CFSIZ, GFP_KERNEL))) {
				kfree(op->frames);
				kfree(op);
				return -ENOMEM;
			}

			/* clear received can_frames to indicate 'nothing received' */
			memset(op->last_frames, 0, msg_head->nframes * CFSIZ);
		} else {
			/* op->frames = NULL due to memset */

			/* even when we have the RX_FILTER_ID case, we need to store the last frame */
			/* for the throttle functionality */

			/* create array for received can_frames */
			if (!(op->last_frames = kmalloc(CFSIZ, GFP_KERNEL))) {
				kfree(op);
				return -ENOMEM;
			}

			/* clear received can_frames to indicate 'nothing received' */
			memset(op->last_frames, 0, CFSIZ);
		}

		op->sk = sk; /* bcm_delete_rx_op() needs this */
		op->ifindex = ifindex;

		init_timer(&op->timer); /* initialize uninitialized (kmalloc) structure */
		init_timer(&op->thrtimer); /* init throttle timer for RX_CHANGED */

		op->timer.function = bcm_rx_timeout_handler; /* handler for rx timeouts */
		op->timer.data = (unsigned long)op; /* timer.data points to this op-structure */

		op->thrtimer.function = bcm_rx_thr_handler; /* handler for RX_CHANGED throttle timeouts */
		op->thrtimer.data = (unsigned long)op; /* timer.data points to this op-structure */
		op->thrtimer.expires = 0; /* mark disabled timer */

		/* add this bcm_op to the list of the tx_ops */
		list_add(&op->list, &bo->rx_ops);

		do_rx_register = 1; /* call can_rx_register() at end of RX_SETUP */

	} /* if ((op = bcm_find_op(&bo->rx_ops, msg_head->can_id, ifindex))) */


	/* check flags */

	op->flags = msg_head->flags;

	if (op->flags & RX_RTR_FRAME) {

		/* no timers in RTR-mode */
		del_timer(&op->thrtimer);
		del_timer(&op->timer);

		/* funny feature in RX(!)_SETUP only for RTR-mode: */
		/* copy can_id into frame BUT without RTR-flag to  */
		/* prevent a full-load-loopback-test ... ;-]       */
		if ((op->flags & TX_CP_CAN_ID) ||
		    (op->frames[0].can_id == op->can_id))
			op->frames[0].can_id = op->can_id & ~CAN_RTR_FLAG;

	} else {
		if (op->flags & SETTIMER) {

			/* set timer value */

			op->ival1   = msg_head->ival1;
			op->j_ival1 = timeval2jiffies(&msg_head->ival1, 1);
			op->ival2   = msg_head->ival2;
			op->j_ival2 = timeval2jiffies(&msg_head->ival2, 1);

			DBG("RX_SETUP: SETTIMER j_ival1=%ld j_ival2=%ld\n",
			    op->j_ival1, op->j_ival2);

			/* disable an active timer due to zero value? */
			if (!op->j_ival1) {
				del_timer(&op->timer);
				DBG("RX_SETUP: disabled timer for rx timeouts.\n");
			}

			/* free currently blocked msgs ? */
			if (op->thrtimer.expires) { /* running throttle timer? */
				DBG("RX_SETUP: unblocking throttled msgs.\n");
				del_timer(&op->thrtimer);
				op->thrtimer.expires = jiffies + 2; /* send blocked msgs hereafter */
				add_timer(&op->thrtimer);
			}
			/* if (op->j_ival2) is zero, no (new) throttling will happen */
			/* see bcm_rx_update_and_send() and bcm_rx_thr_handler()     */
		}

		if ((op->flags & STARTTIMER) && op->j_ival1) {

			del_timer(&op->timer);

			op->timer.expires = jiffies + op->j_ival1;

			DBG("RX_SETUP: adding timer ival1. func=%p data=(%p) exp=0x%08X\n",
			    (char *) op->timer.function,
			    (char *) op->timer.data,
			    (unsigned int) op->timer.expires);

			add_timer(&op->timer);
		}
	}

	/* now we can register for can_ids, if we added a new bcm_op */
	if (do_rx_register) {
		DBG("RX_SETUP: can_rx_register() for can_id <%03X>. rx_op is (%p)\n", op->can_id, op);

		if (ifindex) {
			struct net_device *dev = dev_get_by_index(ifindex);

			if (dev) {
				can_rx_register(dev, op->can_id, BCM_RX_REGMASK, bcm_rx_handler, op, IDENT);
				dev_put(dev);
			}
		} else 
			can_rx_register(NULL, op->can_id, BCM_RX_REGMASK, bcm_rx_handler, op, IDENT);
	}

	return msg_head->nframes * CFSIZ + MHSIZ;
}

static int bcm_tx_send(struct msghdr *msg, int ifindex, struct sock *sk)
{
	struct sk_buff *skb;
	struct net_device *dev;
	int err;

	/* just copy and send one can_frame */

	if (!ifindex) /* we need a real device to send frames */
		return -ENODEV;

	skb = alloc_skb(CFSIZ, GFP_KERNEL);

	if (!skb)
		return -ENOMEM;

	if ((err = memcpy_fromiovec(skb_put(skb, CFSIZ), msg->msg_iov, CFSIZ)) < 0) {
		kfree_skb(skb);
		return err;
	}

	DBG_FRAME("BCM: TX_SEND: sending frame",
		  (struct can_frame *)skb->data);
	dev = dev_get_by_index(ifindex);

	if (!dev) {
		kfree_skb(skb);
		return -ENODEV;
	}

	skb->dev = dev;
	skb->sk  = sk;
	can_send(skb, 1); /* send with loopback */
	dev_put(dev);

	return CFSIZ + MHSIZ;
}

static int bcm_read_op(struct list_head *ops, struct bcm_msg_head *msg_head, int ifindex)
{
	struct bcm_op *op;
	int ret;

	if ((op = bcm_find_op(ops, msg_head->can_id, ifindex))) {

		DBG("TRX_READ: sending status for can_id <%03X>\n", msg_head->can_id);
		/* put current values into msg_head */
		msg_head->flags   = op->flags;
		msg_head->count   = op->count;
		msg_head->ival1   = op->ival1;
		msg_head->ival2   = op->ival2;
		msg_head->nframes = op->nframes;

		bcm_send_to_user(op, msg_head, op->frames, NULL);

		ret = MHSIZ;

	} else {

		DBG("TRX_READ: did not find op for can_id <%03X>\n",
		    msg_head->can_id);
		ret = -EINVAL;
	}

	return ret;
}

/**************************************************/
/* procfs functions                               */
/**************************************************/

static char *bcm_proc_getifname(int ifindex)
{
	struct net_device *dev;

	if (!ifindex)
		return "any";

	dev = __dev_get_by_index(ifindex); /* no usage counting */
	if (dev)
		return dev->name;

	return "???";
}

static int bcm_read_proc(char *page, char **start, off_t off,
			 int count, int *eof, void *data)
{
	int len = 0;
	struct sock *sk = (struct sock *)data;
	struct bcm_opt *bo = bcm_sk(sk);
	struct bcm_op *op;

	len += snprintf(page + len, PAGE_SIZE - len, ">>> socket %p", sk->sk_socket);
	len += snprintf(page + len, PAGE_SIZE - len, " / sk %p", sk);
	len += snprintf(page + len, PAGE_SIZE - len, " / bo %p", bo);
	len += snprintf(page + len, PAGE_SIZE - len, " / dropped %lu", bo->dropped_usr_msgs);
	len += snprintf(page + len, PAGE_SIZE - len, " / bound %s", bcm_proc_getifname(bo->ifindex));
	len += snprintf(page + len, PAGE_SIZE - len, " <<<\n");

	list_for_each_entry(op, &bo->rx_ops, list) {

		unsigned long reduction;

		/* print only active entries & prevent division by zero */
		if (!op->frames_abs)
			continue;

		len += snprintf(page + len, PAGE_SIZE - len, "rx_op: %03X %-5s ",
				op->can_id, bcm_proc_getifname(op->ifindex));
		len += snprintf(page + len, PAGE_SIZE - len, "[%d]%c ",
				op->nframes,(op->flags & RX_CHECK_DLC)?'d':' ');
		if (op->j_ival1)
			len += snprintf(page + len, PAGE_SIZE - len, "timeo=%ld ", op->j_ival1);

		if (op->j_ival2)
			len += snprintf(page + len, PAGE_SIZE - len, "thr=%ld ", op->j_ival2);

		len += snprintf(page + len, PAGE_SIZE - len, "# recv %ld (%ld) => reduction: ",
				op->frames_filtered, op->frames_abs);

		reduction = 100 - (op->frames_filtered * 100) / op->frames_abs;

		len += snprintf(page + len, PAGE_SIZE - len, "%s%ld%%\n",
				(reduction == 100)?"near ":"", reduction);

		if (len > PAGE_SIZE - 200) {
			/* mark output cut off */
			len += snprintf(page + len, PAGE_SIZE - len, "(..)\n");
			break;
		}
	}

	list_for_each_entry(op, &bo->tx_ops, list) {

		len += snprintf(page + len, PAGE_SIZE - len, "tx_op: %03X %s [%d] ",
				op->can_id, bcm_proc_getifname(op->ifindex), op->nframes);
		if (op->j_ival1)
			len += snprintf(page + len, PAGE_SIZE - len, "t1=%ld ", op->j_ival1);

		if (op->j_ival2)
			len += snprintf(page + len, PAGE_SIZE - len, "t2=%ld ", op->j_ival2);

		len += snprintf(page + len, PAGE_SIZE - len, "# sent %ld\n", op->frames_abs);

		if (len > PAGE_SIZE - 100) {
			/* mark output cut off */
			len += snprintf(page + len, PAGE_SIZE - len, "(..)\n");
			break;
		}
	}

	len += snprintf(page + len, PAGE_SIZE - len, "\n");

	*eof = 1;
	return len;
}

/**************************************************/
/* bcm_op handling tx path                        */
/**************************************************/

static void bcm_can_tx(struct bcm_op *op)
{
	struct sk_buff *skb;
	struct net_device *dev;
	struct can_frame *cf = &op->frames[op->currframe];

	DBG_FRAME("BCM: bcm_can_tx: sending frame", cf);

	if (!op->ifindex)
		return; /* no target device -> exit */

	dev = dev_get_by_index(op->ifindex);

	if (!dev)
		return; /* should this bcm_op remove itself here? */

	skb = alloc_skb(CFSIZ,
			in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if (!skb)
		goto out; /* no memory */

	memcpy(skb_put(skb, CFSIZ), cf, CFSIZ);

	skb->dev = dev;
	skb->sk = op->sk;
	can_send(skb, 1); /* send with loopback */

	op->currframe++;
	op->frames_abs++; /* statistics */

	/* reached last frame? */
	if (op->currframe >= op->nframes)
		op->currframe = 0;
 out:
	dev_put(dev);
}

static void bcm_tx_timeout_handler(unsigned long data)
{
	struct bcm_op *op = (struct bcm_op*)data;

	DBG("Called with bcm_op (%p)\n", op);

	if (op->j_ival1 && (op->count > 0)) {

		op->count--;

		if (!op->count && (op->flags & TX_COUNTEVT)) { /* create notification to user? */

			struct bcm_msg_head msg_head;

			DBG("sending TX_EXPIRED for can_id <%03X>\n", op->can_id);

			msg_head.opcode  = TX_EXPIRED;
			msg_head.flags   = op->flags;
			msg_head.count   = op->count;
			msg_head.ival1   = op->ival1;
			msg_head.ival2   = op->ival2;
			msg_head.can_id  = op->can_id;
			msg_head.nframes = 0;

			bcm_send_to_user(op, &msg_head, NULL, NULL);
		}
	}

	DBG("count=%d j_ival1=%ld j_ival2=%ld\n",
	    op->count, op->j_ival1, op->j_ival2);

	if (op->j_ival1 && (op->count > 0)) {

		op->timer.expires = jiffies + op->j_ival1;
		add_timer(&op->timer);

		DBG("adding timer ival1. func=%p data=(%p) exp=0x%08X\n",
		    op->timer.function,
		    (char*) op->timer.data,
		    (unsigned int) op->timer.expires);

		bcm_can_tx(op); /* send (next) frame */
	} else {
		if (op->j_ival2) {
			op->timer.expires = jiffies + op->j_ival2;
			add_timer(&op->timer);

			DBG("adding timer ival2. func=%p data=(%p) exp=0x%08X\n",
			    op->timer.function,
			    (char*) op->timer.data,
			    (unsigned int) op->timer.expires);

			bcm_can_tx(op); /* send (next) frame */
		} else
			DBG("no timer restart\n");
	}

	return;

}

/**************************************************/
/* bcm_op handling rx path                        */
/**************************************************/

static void bcm_rx_handler(struct sk_buff *skb, void *data)
{
	struct bcm_op *op = (struct bcm_op*)data;
	struct can_frame rxframe;
	int i;

	del_timer(&op->timer); /* disable timeout */

	DBG("Called with bcm_op (%p)\n", op);

	if (skb->len == sizeof(rxframe)) {
		memcpy(&rxframe, skb->data, sizeof(rxframe));
		skb_get_timestamp(skb, &op->rx_stamp); /* save rx timestamp */
		op->rx_ifindex = skb->dev->ifindex; /* save originator for recvfrom() */
		op->frames_abs++; /* statistics */
		kfree_skb(skb);
		DBG("got can_frame with can_id <%03X>\n", rxframe.can_id);
	} else {
		DBG("Wrong skb->len = %d\n", skb->len);
		kfree_skb(skb);
		return;
	}

	DBG_FRAME("BCM: bcm_rx_handler: CAN frame", &rxframe);

	if (op->can_id != rxframe.can_id) {
		DBG("ERROR! Got wrong can_id <%03X>! Expected <%03X>.\n",
		    rxframe.can_id, op->can_id);
		return;
	}

	if (op->flags & RX_RTR_FRAME) { /* send reply for RTR-request */
		DBG("RTR-request\n");
		bcm_can_tx(op); /* send op->frames[0] to CAN device */
		return;
	}

	if (op->flags & RX_FILTER_ID) { /* the easiest case */
		DBG("Easy does it with RX_FILTER_ID\n");
		bcm_rx_update_and_send(op, &op->last_frames[0], &rxframe);
		bcm_rx_starttimer(op);
		return;
	}

	if (op->nframes == 1) { /* simple compare with index 0 */
		DBG("Simple compare\n");
		bcm_rx_cmp_to_index(op, 0, &rxframe);
		bcm_rx_starttimer(op);
		return;
	}

	if (op->nframes > 1) { /* multiplex compare */

		DBG("Multiplex compare\n");
		/* find the first multiplex mask that fits */
		/* MUX-mask is in index 0 */

		for (i=1; i < op->nframes; i++) {

			if ((GET_U64(&op->frames[0]) & GET_U64(&rxframe)) ==
			    (GET_U64(&op->frames[0]) & GET_U64(&op->frames[i]))) {
				DBG("found MUX index %d\n", i);
				bcm_rx_cmp_to_index(op, i, &rxframe);
				break;
			}
		}
		bcm_rx_starttimer(op);
	}
}

static void bcm_rx_cmp_to_index(struct bcm_op *op, int index,
				struct can_frame *rxdata)
{
	/* no one uses the MSBs of can_dlc for comparation, */
	/* so we use it here to detect the first time of reception */

	if (!(op->last_frames[index].can_dlc & RX_RECV)) { /* first time? */
		DBG("first time :)\n");
		bcm_rx_update_and_send(op, &op->last_frames[index], rxdata);
		return;
	}

	/* do a real check in can_data */

	DBG("op->frames[index].data = 0x%016llx\n", GET_U64(&op->frames[index]));
	DBG("op->last_frames[index].data = 0x%016llx\n",
	    GET_U64(&op->last_frames[index]));
	DBG("rxdata->data = 0x%016llx\n", GET_U64(rxdata));

	if ((GET_U64(&op->frames[index]) & GET_U64(rxdata)) !=
	    (GET_U64(&op->frames[index]) & GET_U64(&op->last_frames[index]))) {
		DBG("relevant data change :)\n");
		bcm_rx_update_and_send(op, &op->last_frames[index], rxdata);
		return;
	}


	if (op->flags & RX_CHECK_DLC) {

		/* do a real check in dlc */

		if (rxdata->can_dlc != (op->last_frames[index].can_dlc & BCM_CAN_DLC_MASK)) {
			DBG("dlc change :)\n");
			bcm_rx_update_and_send(op, &op->last_frames[index], rxdata);
			return;
		}
	}
	DBG("no relevant change :(\n");
}

static void bcm_rx_update_and_send(struct bcm_op *op,
				   struct can_frame *lastdata,
				   struct can_frame *rxdata)
{
	unsigned long nexttx = op->j_lastmsg + op->j_ival2;

	memcpy(lastdata, rxdata, CFSIZ);
	lastdata->can_dlc |= RX_RECV; /* mark as used */

	/* throttle bcm_rx_changed ? */
	if ((op->thrtimer.expires) || /* somebody else is already waiting OR */
	    ((op->j_ival2) && (nexttx > jiffies))) {      /* we have to wait */

		lastdata->can_dlc |= RX_THR; /* mark as 'throttled' */

		if (!(op->thrtimer.expires)) { /* start only the first time */
			op->thrtimer.expires = nexttx;
			add_timer(&op->thrtimer);

			DBG("adding thrtimer. func=%p data=(%p) exp=0x%08X\n",
			    op->thrtimer.function,
			    (char*) op->thrtimer.data,
			    (unsigned int) op->thrtimer.expires);
		}
	} else
		bcm_rx_changed(op, rxdata); /* send RX_CHANGED to the user */
}

static void bcm_rx_starttimer(struct bcm_op *op)
{
	if (op->flags & RX_NO_AUTOTIMER)
		return;

	if (op->j_ival1) {

		op->timer.expires = jiffies + op->j_ival1;

		DBG("adding rx timeout timer ival1. func=%p data=(%p) exp=0x%08X\n",
		    op->timer.function,
		    (char*) op->timer.data,
		    (unsigned int) op->timer.expires);

		add_timer(&op->timer);
	}
}


static void bcm_rx_changed(struct bcm_op *op, struct can_frame *data)
{
	struct bcm_msg_head head;

	op->j_lastmsg = jiffies;
	op->frames_filtered++; /* statistics */

	if (op->frames_filtered > ULONG_MAX/100)
		op->frames_filtered = op->frames_abs = 0; /* restart - spinlock ? */

	DBG("setting j_lastmsg to 0x%08X for rx_op(%p)\n",
	    (unsigned int) op->j_lastmsg, op);
	DBG("sending notification\n");

	head.opcode  = RX_CHANGED;
	head.flags   = op->flags;
	head.count   = op->count;
	head.ival1   = op->ival1;
	head.ival2   = op->ival2;
	head.can_id  = op->can_id;
	head.nframes = 1;

	bcm_send_to_user(op, &head, data, &op->rx_stamp);
}


static void bcm_rx_timeout_handler(unsigned long data)
{
	struct bcm_op *op = (struct bcm_op*)data;
	struct bcm_msg_head msg_head;

	DBG("sending RX_TIMEOUT for can_id <%03X>. op is (%p)\n", op->can_id, op);

	msg_head.opcode  = RX_TIMEOUT;
	msg_head.flags   = op->flags;
	msg_head.count   = op->count;
	msg_head.ival1   = op->ival1;
	msg_head.ival2   = op->ival2;
	msg_head.can_id  = op->can_id;
	msg_head.nframes = 0;

	bcm_send_to_user(op, &msg_head, NULL, NULL);

	/* no restart of the timer is done here! */

	/* if the user wants to be informed, when cyclic CAN-Messages come back ... */
	if ((op->flags & RX_ANNOUNCE_RESUME) && op->last_frames) {
		/* clear received can_frames to indicate 'nothing received' */
		memset(op->last_frames, 0, op->nframes * CFSIZ);
		DBG("RX_ANNOUNCE_RESTART\n");
	}

}

static void bcm_rx_thr_handler(unsigned long data)
{
	struct bcm_op *op = (struct bcm_op*)data;
	int i = 0;

	op->thrtimer.expires = 0; /* mark disabled / consumed timer */

	if (op->nframes > 1){

		DBG("sending MUX RX_CHANGED for can_id <%03X>. op is (%p)\n",
		    op->can_id, op);
		/* for MUX filter we start at index 1 */
		for (i=1; i<op->nframes; i++){
			if ((op->last_frames) && (op->last_frames[i].can_dlc & RX_THR)){
				op->last_frames[i].can_dlc &= ~RX_THR;
				bcm_rx_changed(op, &op->last_frames[i]);
			}
		}
	} else {

		DBG("sending simple RX_CHANGED for can_id <%03X>. op is (%p)\n",
		    op->can_id, op);
		/* for RX_FILTER_ID and simple filter */
		if (op->last_frames && (op->last_frames[0].can_dlc & RX_THR)){
			op->last_frames[0].can_dlc &= ~RX_THR;
			bcm_rx_changed(op, &op->last_frames[0]);
		}
	}
}

static void bcm_send_to_user(struct bcm_op *op, struct bcm_msg_head *head,
			     struct can_frame *frames, struct timeval *tv)
{
	struct sk_buff *skb;
	struct can_frame *firstframe;
	struct sock *sk = op->sk;
	int datalen = head->nframes * CFSIZ;
	struct sockaddr_can *addr;
	int err;

	skb = alloc_skb(sizeof(*head) + datalen,
			in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (!skb)
		return;

	memcpy(skb_put(skb, sizeof(*head)), head, sizeof(*head));
	firstframe = (struct can_frame *) skb->tail; /* can_frames starting here */

	if (tv)
		skb_set_timestamp(skb, tv); /* restore timestamp */

	addr = (struct sockaddr_can *)skb->cb;
	memset(addr, 0, sizeof(*addr));
	addr->can_family  = AF_CAN;
	addr->can_ifindex = op->rx_ifindex; /* restore originator for recvfrom() */

	if (head->nframes){
		memcpy(skb_put(skb, datalen), frames, datalen);

		/* the BCM uses the can_dlc-element of the can_frame structure
		   for internal purposes. This is only relevant for updates
		   that are generated by the BCM, where nframes is 1
		*/
		if (head->nframes == 1)
			firstframe->can_dlc &= BCM_CAN_DLC_MASK;
	}
	if ((err = sock_queue_rcv_skb(sk, skb)) < 0) {
		struct bcm_opt *bo = bcm_sk(sk);
		DBG("sock_queue_rcv_skb failed: %d\n", err);
		kfree_skb(skb);
		bo->dropped_usr_msgs++; /* don't care about overflows */
	}
}

/**************************************************/
/* bcm_op handling: find & delete bcm_op elements */
/**************************************************/

static struct bcm_op *bcm_find_op(struct list_head *ops, canid_t can_id, int ifindex)
{
	struct bcm_op *op;

	list_for_each_entry(op, ops, list)
		if ((op->can_id == can_id) && (op->ifindex == ifindex))
			return op;

	return NULL;
}

static int bcm_delete_rx_op(struct list_head *ops, canid_t can_id, int ifindex)
{
	struct bcm_op *op, *n;

	list_for_each_entry_safe(op, n, ops, list) {
		if ((op->can_id == can_id) && (op->ifindex == ifindex)) {
			DBG("removing rx_op (%p) for can_id <%03X>\n", op, op->can_id);

			/* Don't care if we're bound or not (due to netdev problems) */
			/* can_rx_unregister() is always a save thing to do here     */
			if (op->ifindex) {
				struct net_device *dev = dev_get_by_index(op->ifindex);
				if (dev) {
					can_rx_unregister(dev, op->can_id, BCM_RX_REGMASK, bcm_rx_handler, op);
					dev_put(dev);
				}
			} else
				can_rx_unregister(NULL, op->can_id, BCM_RX_REGMASK, bcm_rx_handler, op);

			list_del(&op->list);
			bcm_remove_op(op);
			return 1; /* done */
		}
	}

	return 0; /* not found */
}

static int bcm_delete_tx_op(struct list_head *ops, canid_t can_id, int ifindex)
{
	struct bcm_op *op, *n;

	list_for_each_entry_safe(op, n, ops, list) {
		if ((op->can_id == can_id) && (op->ifindex == ifindex)) {
			DBG("removing rx_op (%p) for can_id <%03X>\n",
			    op, op->can_id);
			list_del(&op->list);
			bcm_remove_op(op);
			return 1; /* done */
		}
	}

	return 0; /* not found */
}

static void bcm_remove_op(struct bcm_op *op)
{
	del_timer(&op->timer);
	del_timer(&op->thrtimer);
	if (op->frames)
		kfree(op->frames);
	if (op->last_frames)
		kfree(op->last_frames);
	kfree(op);

	return;
}

module_init(bcm_module_init);
module_exit(bcm_module_exit);
