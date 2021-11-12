/* SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only */
/* Copyright (c) 2017-2021 Brett Sheffield <bacs@librecast.net> */

#ifndef _LIBRECAST_PVT_H
#define _LIBRECAST_PVT_H 1

#include "../include/librecast/types.h"
#include <stddef.h>

typedef struct lc_ctx_t {
	lc_ctx_t *next;
	uint32_t id;
	lc_socket_t *sock_list;
	lc_channel_t *chan_list;
	int sock; /* AF_LOCAL socket for ioctls */
} lc_ctx_t;

#ifndef IPV6_MULTICAST_ALL
typedef struct lc_grplist_s lc_grplist_t;
struct lc_grplist_s {
	lc_grplist_t *next;
	struct in6_addr grp;
};
#endif

typedef struct lc_socket_t {
	lc_socket_t *next;
	lc_ctx_t *ctx;
	pthread_t thread;
	uint32_t id;
	unsigned int ifx; /* interface index, 0 = all (default) */
#ifndef IPV6_MULTICAST_ALL
	lc_grplist_t *grps;
#endif
	int bound; /* how many channels are bound to this socket */
	int sock;
} lc_socket_t;

typedef struct lc_channel_t {
	lc_channel_t *next;
	lc_ctx_t *ctx;
	struct lc_socket_t *sock;
	struct sockaddr_in6 sa;
	char *uri;
	uint32_t id;
	lc_seq_t seq; /* sequence number (Lamport clock) */
	lc_rnd_t rnd; /* random nonce */
} lc_channel_t;

typedef struct lc_message_head_t {
	uint64_t timestamp; /* nanosecond timestamp */
	lc_seq_t seq; /* sequence number */
	lc_rnd_t rnd; /* nonce */
	uint8_t op;
	lc_len_t len;
} __attribute__((__packed__)) lc_message_head_t;

extern uint32_t ctx_id;
extern uint32_t sock_id;
extern uint32_t chan_id;

extern lc_ctx_t *ctx_list;
extern lc_socket_t *sock_list;
extern lc_channel_t *chan_list;

#define BUFSIZE 1500
#define DEFAULT_ADDR "ff1e::"

#endif /* _LIBRECAST_PVT_H */
