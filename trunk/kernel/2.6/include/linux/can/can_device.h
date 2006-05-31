/*
 *	$Id: $
 *
 * DESCRIPTION:
 *	Contains defenition of can_device type and all kernel-only
 *  stuff related with it
 *
 * AUTHOR:
 *  Andrey Volkov <avolkov@varma-el.com>
 *
 * COPYRIGHT:
 *  2006, Varma Electronics Oy
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
 */

#ifndef __CAN_DEVICE_H__
#define __CAN_DEVICE_H__

#include <linux/can/can_error.h>
#include <linux/can/can_ioctl.h>

struct can_device {
	struct net_device_stats stats;
	struct net_device 		*net_dev;

	/* can-bus oscillator frequency, in MHz,
	   BE CAREFUL! SOME CONTROLLERS (LIKE SJA1000)
	   FOOLISH ABOUT THIS FRQ (for sja1000 as ex. this
	   clock must be xtal clock divided by 2). */
	uint8_t					can_sys_clock;

	/* by default max_brp is equal 64,
	   but for a Freescale TouCAN, as ex., it can be 255*/
	uint32_t				max_brp;
	/* For the mostly all controllers, max_sjw is equal 4, but
	   some, hmm, CAN implementations hardwared it to 1 */
	uint8_t					max_sjw;

	uint32_t				baudrate;	/* in bauds */
	struct can_bittime 		bit_time;

	spinlock_t				irq_lock;

	int 					(*do_set_bit_time)(struct can_device *dev,
											   struct can_bittime *br);
	int 					(*do_get_state)(struct can_device *dev,
											enum CAN_STATE *state);
	int 					(*do_set_mode)(struct can_device *dev,
										   enum CAN_MODE mode);
	void   					*priv;
};

#define ND2D(_ndev)		(_ndev->class_dev.dev)
#define CAN2ND(can)		((can)->net_dev)
#define ND2CAN(ndev)	((struct can_device *)netdev_priv(ndev))

struct can_device *alloc_candev(int sizeof_priv);
void free_candev(struct can_device *);

int can_calc_bit_time(struct can_device *can, u32 bit_time_nsec,
					  struct can_bittime_std *bit_time);

#endif /* __CAN_DEVICE_H__ */
