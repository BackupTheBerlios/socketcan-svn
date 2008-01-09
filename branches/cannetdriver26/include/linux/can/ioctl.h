
/*
 * linux/can/ioctl.h
 *
 * Definitions for CAN controller setup (work in progress)
 *
 * $Id$
 *
 * Send feedback to <socketcan-users@lists.berlios.de>
 *
 */

#ifndef CAN_IOCTL_H
#define CAN_IOCTL_H

#include <linux/sockios.h>

/*
 * CAN Baudrate for CAN-controller in bits per second.
 * 0 = Scan for baudrate (Autobaud)
 */
typedef __u32 can_baudrate_t;

/*
 * CAN custom bit time
 */
typedef enum CAN_BITTIME_TYPE {
	CAN_BITTIME_STD,
	CAN_BITTIME_BTR
} can_bittime_type_t;

/* TSEG1 of controllers usually is a sum of synch_seg (always 1),
 * prop_seg and phase_seg1, TSEG2 = phase_seg2 */

struct can_bittime_std {
	__u32 brp;        /* baud rate prescaler */
	__u8  prop_seg;   /* from 1 to 8 */
	__u8  phase_seg1; /* from 1 to 8 */
	__u8  phase_seg2; /* from 1 to 8 */
	__u8  sjw:7;      /* from 1 to 4 */
	__u8  sam:1;      /* 1 - enable triple sampling */
};

struct can_bittime_btr {
	__u8  btr0;
	__u8  btr1;
};

struct can_bittime {
	can_bittime_type_t type;
	union {
		struct can_bittime_std std;
		struct can_bittime_btr btr;
	};
};

#define CAN_BAUDRATE_UNCONFIGURED	((__u32) 0xFFFFFFFFU)
#define CAN_BAUDRATE_UNKNOWN		0

/*
 * CAN mode
 */
typedef __u32 can_mode_t;

#define CAN_MODE_STOP	0
#define CAN_MODE_START	1
#define CAN_MODE_SLEEP	2

/*
 * CAN controller mode
 */
typedef __u32 can_ctrlmode_t;

#define CAN_CTRLMODE_LOOPBACK   0x1
#define CAN_CTRLMODE_LISTENONLY 0x2

/*
 * CAN operational and error states
 */
typedef __u32 can_state_t;

#define CAN_STATE_ACTIVE		0
#define CAN_STATE_BUS_WARNING		1
#define CAN_STATE_BUS_PASSIVE		2
#define CAN_STATE_BUS_OFF		3
#define CAN_STATE_STOPPED		5
#define CAN_STATE_SLEEPING		6

/*
 * CAN device statistics
 */
struct can_device_stats {
	int error_warning;
	int data_overrun;
	int wakeup;
	int bus_error;
	int error_passive;
	int arbitration_lost;
	int restarts;
	int bus_error_at_init;
};

#endif /* CAN_IOCTL_H */
