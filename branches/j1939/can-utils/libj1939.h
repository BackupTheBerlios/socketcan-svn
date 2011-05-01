#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/j1939.h>

#ifndef J1939_LIB_H
#define J1939_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

extern int libj1939_ifindex(const char *ifname);
extern const char *libj1939_ifnam(int ifindex);
/*
 * cleanup held resources
 * this is a __attribute__((destructor)), so not calling this
 * is ok too.
 */
extern void libj1939_cleanup(void);

extern int libj1939_str2addr(const char *str, char **endp, struct sockaddr_can *can);
extern const char *libj1939_addr2str(const struct sockaddr_can *can);

#ifdef __cplusplus
}
#endif

#endif
