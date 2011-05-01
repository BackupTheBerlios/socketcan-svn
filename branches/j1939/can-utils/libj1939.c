/*
 * Copyright (c) 2011 EIA Electronics
 *
 * Authors:
 * Kurt Van Dijck <kurt.van.dijck@eia.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the version 2 of the GNU General Public License
 * as published by the Free Software Foundation
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>

#include <error.h>
#include <unistd.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include "libj1939.h"

struct ifname {
	struct ifname *next;
	int ifindex;
	char name[2];
};

/* static data */
static struct {
	int sock;
	struct ifname *names;
} s = {
	.sock = -1,
};

__attribute__((destructor)) void libj1939_cleanup(void)
{
	struct ifname *nam;

	if (s.sock >= 0)
		close(s.sock);
	s.sock = -1;

	while (s.names) {
		nam = s.names;
		s.names = nam->next;
		free(nam);
	}
}

static void verify_sock(void)
{
	if (s.sock >= 0)
		return;
	s.sock = socket(PF_CAN, SOCK_DGRAM, CAN_J1939);
	if (s.sock < 0)
		error(1, errno, "sock(can, dgram, j1939)");
	fcntl(s.sock, F_SETFD, fcntl(s.sock, F_GETFD) | FD_CLOEXEC);
}

static struct ifname *libj1939_add_ifnam(int ifindex, const char *str)
{
	struct ifname *nam;
	nam = malloc(sizeof(*nam) + strlen(str));
	memset(nam, 0, sizeof(*nam));
	nam->ifindex = ifindex;
	strcpy(nam->name, str);
	nam->next = s.names;
	s.names = nam;
	return nam;
}

/* retrieve name */
const char *libj1939_ifnam(int ifindex)
{
	int ret;

	const struct ifname *lp;
	struct ifname *nam;
	struct ifreq ifr;

	for (lp = s.names; lp; lp = lp->next) {
		if (lp->ifindex == ifindex)
			return lp->name;
	}
	/* find out this new ifindex */
	verify_sock();
	ifr.ifr_ifindex = ifindex;
	ret = ioctl(s.sock, SIOCGIFNAME, &ifr);
	if (ret < 0)
		error(1, errno, "get ifname(%u)", ifindex);
	nam = libj1939_add_ifnam(ifindex, ifr.ifr_name);
	return nam ? nam->name : 0;
}

/* retrieve index */
int libj1939_ifindex(const char *str)
{
	const struct ifname *lp;
	struct ifname *nam;
	char *endp;
	int ret;
	struct ifreq ifr;

	ret = strtol(str, &endp, 0);
	if (!*endp)
		/* did some good parse */
		return ret;

	for (lp = s.names; lp; lp = lp->next) {
		if (!strcmp(lp->name, str))
			return lp->ifindex;
	}
	/* find out this new ifindex */
	verify_sock();
	strncpy(ifr.ifr_name, str, sizeof(ifr.ifr_name));
	ret = ioctl(s.sock, SIOCGIFINDEX, &ifr);
	if (ret < 0)
		error(1, errno, "get ifindex(%s)", str);
	nam = libj1939_add_ifnam(ifr.ifr_ifindex, str);
	return nam ? nam->ifindex : 0;
}

int libj1939_str2addr(const char *str, char **endp, struct sockaddr_can *can)
{
	char *p;
	const char *pstr;
	uint64_t tmp64;
	unsigned long tmp;

	if (!endp)
		endp = &p;
	memset(can, 0, sizeof(*can));
	can->can_addr.j1939.name = J1939_NO_NAME;
	can->can_addr.j1939.addr = J1939_NO_ADDR;
	can->can_addr.j1939.pgn = J1939_NO_PGN;

	pstr = strchr(str, ':');
	if (pstr) {
		char tmp[IFNAMSIZ];
		if ((pstr - str) >= IFNAMSIZ)
			return -1;
		strncpy(tmp, str, pstr - str);
		tmp[pstr - str] = 0;
		can->can_ifindex = libj1939_ifindex(tmp);
	}
	if (pstr)
		++pstr;
	else
		pstr = str;


	tmp64 = strtoull(pstr, endp, 16);
	if (*endp <= pstr)
		return 0;
	if ((*endp - pstr) == 2)
		can->can_addr.j1939.addr = tmp64;
	else
		can->can_addr.j1939.name = tmp64;
	if (!**endp)
		return 0;

	str = *endp + 1;
	tmp = strtoul(str, endp, 16);
	if (*endp > str)
		can->can_addr.j1939.pgn = tmp;
	return 0;
}

const char *libj1939_addr2str(const struct sockaddr_can *can)
{
	char *str;
	static char buf[128];

	str = buf;
	if (can->can_ifindex) {
		const char *ifname;
		ifname = libj1939_ifnam(can->can_ifindex);
		if (!ifname)
			str += sprintf(str, "#%i:", can->can_ifindex);
		else
			str += sprintf(str, "%s:", ifname);
	}
	if (can->can_addr.j1939.name) {
		str += sprintf(str, "%016llx", (unsigned long long)can->can_addr.j1939.name);
		if (can->can_addr.j1939.pgn == 0x0ee00)
			str += sprintf(str, ".%02x", can->can_addr.j1939.addr);
	} else if (can->can_addr.j1939.addr <= 0xfe)
		str += sprintf(str, "%02x", can->can_addr.j1939.addr);
	else
		str += sprintf(str, "-");
	if (can->can_addr.j1939.pgn <= 0x3ffff)
		str += sprintf(str, ",%05x", can->can_addr.j1939.pgn);

	return buf;
}

