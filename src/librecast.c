/* SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only */
/* Copyright (c) 2017-2022 Brett Sheffield <bacs@librecast.net> */

#define _GNU_SOURCE
#include "librecast_pvt.h"
#include <librecast/net.h>
#include "hash.h"
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

uint32_t ctx_id = 0;
uint32_t sock_id = 0;
uint32_t chan_id = 0;

lc_ctx_t *ctx_list = NULL;

static void lc_op_data_handler(lc_socket_call_t *sc, lc_message_t *msg);
static void lc_op_ping_handler(lc_socket_call_t *sc, lc_message_t *msg);
static void lc_op_pong_handler(lc_socket_call_t *sc, lc_message_t *msg);

int (*lc_msg_logger)(lc_channel_t *, lc_message_t *, void *logdb) = NULL;

void (*lc_op_handler[LC_OP_MAX])(lc_socket_call_t *, lc_message_t *) = {
	lc_op_data_handler,
	lc_op_ping_handler,
	lc_op_pong_handler,
};

int lc_getrandom(void *buf, size_t buflen)
{
	int err, fd;

	if ((fd = open("/dev/urandom", O_RDONLY)) == -1) return -1;
	err = read(fd, buf, buflen);
	close(fd);

	return err;
}

uint32_t lc_ctx_get_id(lc_ctx_t *ctx)
{
	return (ctx) ? ctx->id : 0;
}

uint32_t lc_socket_get_id(lc_socket_t *sock)
{
	return (sock) ? sock->id : 0;
}

uint32_t lc_channel_get_id(lc_channel_t *chan)
{
	return (chan) ? chan->id : 0;
}

lc_ctx_t *lc_channel_ctx(lc_channel_t *chan)
{
	return chan->ctx;
}

lc_socket_t *lc_channel_socket(lc_channel_t *chan)
{
	return chan->sock;
}

char *lc_channel_uri(lc_channel_t *chan)
{
	return chan->uri;
}

struct in6_addr *lc_channel_in6addr(lc_channel_t *chan)
{
	return &(chan->sa.sin6_addr);
}

struct sockaddr_in6 *lc_channel_sockaddr(lc_channel_t *chan)
{
	return &chan->sa;
}

int lc_channel_socket_raw(lc_channel_t *chan)
{
	return chan->sock->sock;
}

int lc_socket_raw(lc_socket_t *sock)
{
	return sock->sock;
}

void *lc_msg_data(lc_message_t *msg)
{
	return (msg) ? msg->data: NULL;
}

int lc_msg_get(lc_message_t *msg, lc_msg_attr_t attr, void **value)
{
	if (!msg || !value) return LC_ERROR_INVALID_PARAMS;
	switch (attr) {
	case LC_ATTR_DATA:
		*value = msg->data;
		break;
	case LC_ATTR_LEN:
		*value = (void *)&msg->len;
		break;
	case LC_ATTR_OPCODE:
		*value = (void *)&msg->op;
		break;
	default:
		return LC_ERROR_MSG_ATTR_UNKNOWN;
	}
	return 0;
}

int lc_msg_set(lc_message_t *msg, lc_msg_attr_t attr, void *value)
{
	if (!msg) return LC_ERROR_INVALID_PARAMS;
	switch (attr) {
	case LC_ATTR_DATA:
		msg->data = value;
		break;
	case LC_ATTR_LEN:
		msg->len = *(lc_len_t *)value;
		break;
	case LC_ATTR_OPCODE:
		msg->op = *(lc_opcode_t *)value;
		break;
	default:
		return LC_ERROR_MSG_ATTR_UNKNOWN;
	}
	return 0;
}

int lc_msg_id(lc_message_t *msg, unsigned char *id, size_t len)
{
	hash_state state;

	hash_init(&state, NULL, 0, len);
	hash_update(&state, (unsigned char *)msg->data, msg->len);
	hash_update(&state, (unsigned char *)msg->srcaddr, sizeof(struct in6_addr));
	hash_final(&state, id, len);

	return 0;
}

int lc_socket_getopt(lc_socket_t *sock, int optname, void *optval, socklen_t *optlen)
{
	return getsockopt(sock->sock, IPPROTO_IPV6, optname, optval, optlen);
}

int lc_socket_setopt(lc_socket_t *sock, int optname, const void *optval, socklen_t optlen)
{
	return setsockopt(sock->sock, IPPROTO_IPV6, optname, optval, optlen);
}

int lc_socket_loop(lc_socket_t *sock, int val)
{
	return setsockopt(sock->sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &val, sizeof val);
}

int lc_socket_ttl(lc_socket_t *sock, int val)
{
	return setsockopt(sock->sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &val, sizeof val);
}

static void *_free(void *msg, void *hint)
{
	free(msg);
	return hint;
}

void lc_msg_free(void *arg)
{
	lc_message_t *msg = (lc_message_t *)arg;
	if (msg->free) {
		msg->free(msg->data, msg->hint);
		msg->data = NULL;
	}
}

void *lc_msg_init(lc_message_t *msg)
{
	return memset(msg, 0, sizeof(lc_message_t));
}

int lc_msg_init_data(lc_message_t *msg, void *data, size_t len, lc_free_fn_t *f, void *hint)
{
	lc_msg_init(msg);
	msg->len = len;
	msg->data = data;
	msg->free = f;
	msg->hint = hint;
	return 0;
}

int lc_msg_init_size(lc_message_t *msg, size_t len)
{
	lc_msg_init(msg);
	msg->data = malloc(len);
	if (!msg->data) return -1;
	msg->len = len;
	msg->free = &_free;
	return 0;
}

void lc_channel_free(lc_channel_t * chan)
{
	if (!chan) return;
	for (lc_channel_t *p = chan->ctx->chan_list, *prev = NULL; p; p = p->next) {
		if (p->id == chan->id) {
			if (prev) prev->next = p->next;
			else chan->ctx->chan_list = p->next;
		}
		prev = p;
	}
	free(chan);
}

ssize_t lc_channel_sendmsg(lc_channel_t *chan, struct msghdr *msg, int flags)
{
	msg->msg_name = (struct sockaddr *)&chan->sa;
	msg->msg_namelen = sizeof(struct sockaddr_in6);
	return sendmsg(chan->sock->sock, msg, flags);
}

ssize_t lc_channel_send(lc_channel_t *chan, const void *buf, size_t len, int flags)
{
	return sendto(chan->sock->sock, buf, len, flags,
		(struct sockaddr *)&chan->sa, sizeof(struct sockaddr_in6));
}

ssize_t lc_socket_sendmsg(lc_socket_t *sock, struct msghdr *msg, int flags)
{
	ssize_t bytes = 0, rc;
	for (lc_channel_t *chan = sock->ctx->chan_list; chan; chan = chan->next) {
		if (chan->sock == sock) {
			if ((rc = lc_channel_sendmsg(chan, msg, flags)) > 0) {
				bytes += rc;
			}
			else return -1;
		}
	}
	return bytes;
}

ssize_t lc_socket_send(lc_socket_t *sock, const void *buf, size_t len, int flags)
{
	ssize_t bytes = 0, rc;
	for (lc_channel_t *chan = sock->ctx->chan_list; chan; chan = chan->next) {
		if (chan->sock == sock) {
			if ((rc = lc_channel_send(chan, buf, len, flags)) > 0) {
				bytes += rc;
			}
			else return -1;
		}
	}
	return bytes;
}

ssize_t lc_msg_sendto(int sock, const void *buf, size_t len, struct sockaddr_in6 *sa, int flags)
{
	return sendto(sock, buf, len, flags, (struct sockaddr *)sa, sizeof(struct sockaddr_in6));
}

ssize_t lc_msg_send(lc_channel_t *chan, lc_message_t *msg)
{
	struct sockaddr_in6 *sa = &chan->sa;
	lc_message_head_t *head = NULL;
	char *buf = NULL;
	size_t len = 0;
	ssize_t bytes = 0;
	struct timespec t = {0};
	int state = 0;
	int err = 0;

	if (!chan->sock) return LC_ERROR_SOCKET_REQUIRED;
	if (msg->len > 0 && !msg->data) return LC_ERROR_MESSAGE_EMPTY;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state);

	head = calloc(1, sizeof(lc_message_head_t));
	if (!head) return LC_ERROR_MALLOC;

	if (msg->timestamp)
		head->timestamp = htobe64(msg->timestamp);
	else if (!clock_gettime(CLOCK_REALTIME, &t))
		head->timestamp = htobe64(t.tv_sec * 1000000000 + t.tv_nsec);

	head->seq = htobe64(++chan->seq);
	lc_getrandom(&head->rnd, sizeof(lc_rnd_t));
	head->len = htobe64(msg->len);
	head->op = msg->op;
	len = msg->len;
	buf = calloc(1, sizeof(lc_message_head_t) + len);
	if (!buf) {
		free(head);
		return LC_ERROR_MALLOC;
	}
	memcpy(buf, head, sizeof(lc_message_head_t));
	memcpy(buf + sizeof(lc_message_head_t), msg->data, len);
	len += sizeof(lc_message_head_t);

	bytes = lc_msg_sendto(chan->sock->sock, buf, len, sa, 0);
	if (bytes == -1) err = errno;

	free(head);
	free(buf);
	pthread_setcancelstate(state, NULL);

	if (err) errno = err;
	return bytes;
}

#ifndef IPV6_MULTICAST_ALL
static int lc_socket_group_joined(lc_socket_t *sock, struct in6_addr *grp)
{
	for (lc_grplist_t *g = sock->grps; g; g = g->next) {
		if (!memcmp(&g->grp, grp, sizeof(struct in6_addr))) return 1;
	}
	return 0;
}
#endif

ssize_t lc_msg_recv(lc_socket_t *sock, lc_message_t *msg)
{
	ssize_t zi = 0, err = 0;
	struct iovec iov[2];
	struct msghdr msgh = {0};
	char buf[sizeof(lc_message_head_t)];
	char cmsgbuf[BUFSIZE];
	struct sockaddr_in6 from;
	socklen_t fromlen = sizeof(from);
	struct cmsghdr *cmsg;
	lc_message_head_t head;

#ifndef IPV6_MULTICAST_ALL
recv_again:
#endif
	zi = recv(sock->sock, NULL, 0, MSG_PEEK | MSG_TRUNC);
	if (zi == -1) return -1;

	if ((size_t)zi > sizeof(lc_message_head_t)) {
		err = lc_msg_init_size(msg, (size_t)zi - sizeof(lc_message_head_t));
		if (err) return LC_ERROR_MALLOC;
	}

	iov[0].iov_base = buf;
	iov[0].iov_len = sizeof(lc_message_head_t);
	iov[1].iov_base = msg->data;
	iov[1].iov_len = msg->len;
	msgh.msg_control = cmsgbuf;
	msgh.msg_controllen = BUFSIZE;
	msgh.msg_name = &from;
	msgh.msg_namelen = fromlen;
	msgh.msg_iov = iov;
	msgh.msg_iovlen = 2;
	msgh.msg_flags = 0;

	pthread_testcancel();
	if ((zi = recvmsg(sock->sock, &msgh, 0)) <= 0) return zi;
	memcpy(&head, buf, sizeof(lc_message_head_t));
	msg->seq = be64toh(head.seq);
	msg->rnd = be64toh(head.rnd);
	msg->len = be64toh(head.len);
	msg->timestamp = be64toh(head.timestamp);
	msg->op = head.op;
	for (cmsg = CMSG_FIRSTHDR(&msgh); cmsg; cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
		if (cmsg->cmsg_type == IPV6_PKTINFO) {
			/* may not be aligned, copy */
			memcpy(&msg->dst, CMSG_DATA(cmsg), sizeof(struct in6_addr));
			msg->src = (&from)->sin6_addr;
#ifndef IPV6_MULTICAST_ALL
			/* destination is group we haven't joined - drop it */
			if (!lc_socket_group_joined(sock, &msg->dst)) goto recv_again;
#endif
			break;
		}
	}
	return zi;
}

int lc_socket_listen_cancel(lc_socket_t *sock)
{
	if (sock->thread) {
		if (pthread_cancel(sock->thread))
			return LC_ERROR_THREAD_CANCEL;
		if (pthread_join(sock->thread, NULL))
			return LC_ERROR_THREAD_JOIN;
		sock->thread = 0;
	}
	return 0;
}

static void lc_op_pong_handler(lc_socket_call_t *sc, lc_message_t *msg)
{
	if (sc->callback_msg) sc->callback_msg(msg);
}

static void lc_op_ping_handler(lc_socket_call_t *sc, lc_message_t *msg)
{
	(void) sc; /* unused */
	int opt = LC_OP_PONG;

	/* received PING, echo PONG back to same channel */
	lc_msg_set(msg, LC_ATTR_OPCODE, &opt);
	lc_msg_send(msg->chan, msg);
}

static void lc_op_data_handler(lc_socket_call_t *sc, lc_message_t *msg)
{
	/* callback to message handler */
	if (sc->callback_msg) sc->callback_msg(msg);
}

lc_channel_t *lc_channel_by_address(lc_ctx_t *lctx, struct in6_addr *addr)
{
	for (lc_channel_t *p = lctx->chan_list; p; p = p->next) {
		if (!memcmp(addr,& p->sa.sin6_addr, sizeof(struct in6_addr)))
			return p;
	}
	return NULL;
}

static ssize_t lc_socket_recvmsg_if(lc_socket_t *sock, struct msghdr *msg, int flags)
{
	struct in6_pktinfo pi = {0};
	char ctl[CMSG_SPACE(sizeof pi)];
	struct cmsghdr *cmsg;
	ssize_t bytes;
	int opt = 1;

	/* We're only interested in packets arriving on the socket->ifx
	 * interface. If we bind to an interface-specific address, we will get no
	 * multicast packets. If bound to either INADDR_ANY or the multicast
	 * group address, we receive packets on all interfaces. So, we need to
	 * filter by extracting the receiving interface from ancillary data */

	setsockopt(sock->sock, IPPROTO_IPV6, IPV6_RECVPKTINFO, &opt, sizeof opt);

	/* provide control buffer if caller hasn't */
	if (!msg->msg_control) {
		msg->msg_control = ctl;
		msg->msg_controllen = sizeof ctl;
	}

	for (;;) {
		bytes = recvmsg(sock->sock, msg, flags);
		for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
			if (cmsg->cmsg_type == IPV6_PKTINFO) {
				memcpy(&pi, CMSG_DATA(cmsg), sizeof pi);
				if (sock->ifx == pi.ipi6_ifindex) return bytes;
			}
		}
	}
	/* not reached */
}

static ssize_t lc_socket_recv_if(lc_socket_t *sock, void *buf, size_t len, int flags)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	return lc_socket_recvmsg_if(sock, &msg, flags);
}

ssize_t lc_socket_recvmsg(lc_socket_t *sock, struct msghdr *msg, int flags)
{
	if (sock->ifx) return lc_socket_recvmsg_if(sock, msg, flags);
	return recvmsg(sock->sock, msg, flags);
}

ssize_t lc_socket_recv(lc_socket_t *sock, void *buf, size_t len, int flags)
{
	if (sock->ifx) return lc_socket_recv_if(sock, buf, len, flags);
	return recv(sock->sock, buf, len, flags);
}

static void process_msg(lc_socket_call_t *sc, lc_message_t *msg)
{
	lc_channel_t *chan;

	inet_ntop(AF_INET6, &msg->dst, msg->dstaddr, INET6_ADDRSTRLEN);
	inet_ntop(AF_INET6, &msg->src, msg->srcaddr, INET6_ADDRSTRLEN);
	msg->sockid = sc->sock->id;

	/* update channel stats */
	chan = lc_channel_by_address(sc->sock->ctx, &msg->dst);
	if (chan) {
		msg->chan = chan;
		chan->seq = (msg->seq > chan->seq) ? msg->seq + 1 : chan->seq + 1;
		chan->rnd = msg->rnd;
		if (lc_msg_logger) lc_msg_logger(chan, msg, NULL);
	}

	/* opcode handler */
	if (msg->op < LC_OP_MAX && lc_op_handler[msg->op])
		lc_op_handler[msg->op](sc, msg);

	/* callback to message handler */
	if (sc->callback_msg) sc->callback_msg(msg);
}

void *lc_socket_listen_thread(void *arg)
{
	ssize_t len;
	lc_message_t msg = {0};
	lc_socket_call_t *sc = arg;

	pthread_cleanup_push(free, arg);
	pthread_cleanup_push(lc_msg_free, &msg);
	while(1) {
		len = lc_msg_recv(sc->sock, &msg);
		if (len > 0) {
			msg.bytes = len;
			process_msg(sc, &msg);
		}
		if (len < 0) {
			lc_msg_free(&msg);
			if (sc->callback_err) sc->callback_err(len);
		}
		lc_msg_free(&msg);
	}
	/* not reached */
	pthread_cleanup_pop(0);
	pthread_cleanup_pop(0);

	return NULL;
}

int lc_socket_listen(lc_socket_t *sock, void (*callback_msg)(lc_message_t*),
					void (*callback_err)(int))
{
	pthread_attr_t attr = {0};
	lc_socket_call_t *sc;

	if (!sock) return LC_ERROR_SOCKET_REQUIRED;
	if (sock->thread) return LC_ERROR_SOCKET_LISTENING;

	sc = calloc(1, sizeof(lc_socket_call_t));
	if (!sc) return LC_ERROR_MALLOC;
	sc->sock = sock;
	sc->callback_msg = callback_msg;
	sc->callback_err = callback_err;

	pthread_attr_init(&attr);
	pthread_create(&sock->thread, &attr, &lc_socket_listen_thread, sc);
	pthread_attr_destroy(&attr);

	return 0;
}

static int lc_channel_membership_all(int sock, int opt, struct ipv6_mreq *req)
{
	struct ifaddrs *ifaddr, *ifa;
	int rc = (opt == IPV6_JOIN_GROUP) ? LC_ERROR_MCAST_JOIN : LC_ERROR_MCAST_PART;

	if (getifaddrs(&ifaddr) == -1) return -1;
	for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
		if ((ifa->ifa_flags & IFF_MULTICAST) != IFF_MULTICAST
		  || ifa->ifa_addr == NULL
		  || ifa->ifa_addr->sa_family != AF_INET6) continue;

		req->ipv6mr_interface = if_nametoindex(ifa->ifa_name);

		if (!setsockopt(sock, IPPROTO_IPV6, opt, req, sizeof(struct ipv6_mreq))) {
			rc = 0; /* report success if we joined anything */
		}
	}
	freeifaddrs(ifaddr);

	return rc;
}

#ifndef IPV6_MULTICAST_ALL
static void lc_socket_group_add(lc_socket_t *sock, struct in6_addr *grp)
{
	lc_grplist_t *g, *newgrp;
	for (g = sock->grps; g; g = g->next) {
		if (!memcmp(&g->grp, grp, sizeof(struct in6_addr))) return;
		if (!g->next) break;
	}
	newgrp = calloc(1, sizeof(struct lc_grplist_s));
	memcpy(&newgrp->grp, grp, sizeof(struct in6_addr));
	if (!sock->grps) sock->grps = newgrp;
	else g->next = newgrp;
}

static void lc_socket_group_del(lc_socket_t *sock, struct in6_addr *grp)
{
	lc_grplist_t *prev = NULL;
	for (lc_grplist_t *g = sock->grps; g; g = g->next) {
		if (memcmp(g, grp, sizeof(struct in6_addr)) == 0) {
			if (prev) {
				prev->next = (g->next) ? g->next : NULL;
			}
			if (g == sock->grps) sock->grps = NULL;
			free(g);
		}
		prev = g;
	}
}
#endif

static int lc_channel_membership(lc_channel_t *chan, int opt, struct ipv6_mreq *req)
{
	int s = chan->sock->sock;
#ifndef IPV6_MULTICAST_ALL
	if (opt == IPV6_JOIN_GROUP) {
		lc_socket_group_add(chan->sock, &chan->sa.sin6_addr);
	}
	else {
		lc_socket_group_del(chan->sock, &chan->sa.sin6_addr);
	}
#endif
	if (chan->sock->ifx) {
		req->ipv6mr_interface = chan->sock->ifx;
		return setsockopt(s, IPPROTO_IPV6, opt, req, sizeof(struct ipv6_mreq));
	}
	return lc_channel_membership_all(s, opt, req);
}

static int lc_channel_action(lc_channel_t *chan, int opt)
{
	struct ipv6_mreq req = {0};

	if(!chan->sock) return LC_ERROR_SOCKET_REQUIRED;

	memcpy(&req.ipv6mr_multiaddr, &chan->sa.sin6_addr, sizeof(struct in6_addr));

	return lc_channel_membership(chan, opt, &req);
}

int lc_channel_part(lc_channel_t *chan)
{
	return lc_channel_action(chan, IPV6_LEAVE_GROUP);
}

int lc_channel_join(lc_channel_t *chan)
{
	return lc_channel_action(chan, IPV6_JOIN_GROUP);
}

int lc_channel_unbind(lc_channel_t *chan)
{
	chan->sock->bound--;
	chan->sock = NULL;
	return 0;
}

static int lc_socket_bind_addr(lc_socket_t *sock, short port)
{
	int opt = 1;
	struct sockaddr_in6 any = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_ANY_INIT,
		.sin6_port = port
	};

	if ((setsockopt(sock->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) == -1)
		return LC_ERROR_SETSOCKOPT;

#ifdef SO_REUSEPORT
	if ((setsockopt(sock->sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) == -1)
		return LC_ERROR_SETSOCKOPT;
#endif

	if (bind(sock->sock, (struct sockaddr *)&any, sizeof(struct sockaddr_in6)) == -1) {
		/* ignore EINVAL "socket already bound" error */
		if (errno != EINVAL) return LC_ERROR_SOCKET_BIND;
	}

	return 0;
}

int lc_channel_bind(lc_socket_t *sock, lc_channel_t *chan)
{
	/* Librecast sockets can have multiple channels bound to them, but we
	 * only need to call lc_socket_bind_addr() the first time */

	int rc = (sock->bound) ? 0 : lc_socket_bind_addr(sock, chan->sa.sin6_port);

	if (!rc) {
		chan->sock = sock;
		sock->bound++;
	}

	return rc;
}

static int lc_hashgroup(char *baseaddr, unsigned char *group, size_t len,
		struct in6_addr *addr, unsigned int flags)
{
	unsigned char hashgrp[HASHSIZE];
	hash_state state;

	hash_init(&state, NULL, 0, HASHSIZE);
	hash_update(&state, (unsigned char *)group, len);
	hash_update(&state, (unsigned char *)&flags, sizeof(flags));
	hash_final(&state, hashgrp, HASHSIZE);

	/* we have 112 bits (14 bytes) available for the group address
	 * XOR the hashed group with the base multicast address */
	if (inet_pton(AF_INET6, baseaddr, &addr->s6_addr) != 1)
		return LC_ERROR_INVALID_BASEADDR;
	for (int i = 2; i < 16; i++) {
		addr->s6_addr[i] ^= hashgrp[i];
	}

	return 0;
}

static lc_channel_t * lc_channel_ins(lc_ctx_t *ctx, lc_channel_t *chan)
{
	chan->next = ctx->chan_list;
	ctx->chan_list = chan;
	return chan;
}

static inline void lc_channel_setid(lc_channel_t *chan)
{
	chan->id = ++chan_id;
}

lc_channel_t * lc_channel_sidehash(lc_channel_t *base, unsigned char *key, size_t keylen)
{
	struct in6_addr *in;
	unsigned char *ptr;
	lc_ctx_t *ctx = base->ctx;
	lc_channel_t *side = lc_channel_copy(ctx, base);
	if (!side) return NULL;
	in = &side->sa.sin6_addr;
	ptr = (unsigned char *)&in->s6_addr[2];
	hash_generic_key(ptr, 14, (unsigned char *)in, sizeof(struct in6_addr), key, keylen);
	return side;
}

lc_channel_t * lc_channel_sideband(lc_channel_t *base, uint64_t band)
{
	struct in6_addr *in;
	uint64_t *ptr;
	lc_ctx_t *ctx = base->ctx;
	lc_channel_t *side = lc_channel_copy(ctx, base);
	if (!side) return NULL;
	in = &side->sa.sin6_addr;
	ptr = (uint64_t *)&in->s6_addr[8];
	*ptr = band;
	return side;
}

lc_channel_t * lc_channel_copy(lc_ctx_t *ctx, lc_channel_t *chan)
{
	lc_channel_t *copy = calloc(1, sizeof(lc_channel_t));
	if (!copy) return NULL;
	copy->ctx = ctx;
	lc_channel_setid(copy);
	memcpy(&copy->sa, &chan->sa, sizeof(struct sockaddr_in6));
	return lc_channel_ins(ctx, copy);
}

lc_channel_t *lc_channel_init(lc_ctx_t *ctx, struct sockaddr_in6 *sa)
{
	lc_channel_t *chan;
	chan = calloc(1, sizeof(lc_channel_t));
	if (!chan) return NULL;
	chan->ctx = ctx;
	lc_channel_setid(chan);
	memcpy(&chan->sa, sa, sizeof(struct sockaddr_in6));
	return lc_channel_ins(ctx, chan);
}

lc_channel_t * lc_channel_nnew(lc_ctx_t *ctx, unsigned char *s, size_t len)
{
	struct sockaddr_in6 sa = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(LC_DEFAULT_PORT)
	};

	if (lc_hashgroup(DEFAULT_ADDR, s, len, &sa.sin6_addr, 0))
		return NULL;

	return lc_channel_init(ctx, &sa);
}

lc_channel_t * lc_channel_new(lc_ctx_t *ctx, char *s)
{
	lc_channel_t *chan;
	chan = lc_channel_nnew(ctx, (unsigned char *)s, strlen(s));
	chan->uri = s;
	return chan;
}

lc_channel_t *lc_channel_random(lc_ctx_t *ctx)
{
	unsigned char buf[14];
	if (lc_getrandom(buf, sizeof buf) != sizeof buf) return NULL;
	return lc_channel_nnew(ctx, buf, sizeof buf);
}

#ifndef IPV6_MULTICAST_ALL
static int lc_socket_groups_free(lc_socket_t *sock)
{
	lc_grplist_t *tmp;
	for (lc_grplist_t *g = sock->grps; g; ) {
		tmp = g;
		g = g->next;
		free(tmp);
	}
	return 0;
}
#endif

void lc_socket_close(lc_socket_t *sock)
{
	if (!sock) return;

	lc_socket_listen_cancel(sock);
#ifndef IPV6_MULTICAST_ALL
	lc_socket_groups_free(sock);
#endif

	if (sock->sock) close(sock->sock);
	lc_socket_t *prev = NULL;
	for (lc_socket_t *p = sock->ctx->sock_list; p; p = p->next) {
		if (p->id == sock->id) {
			if (prev) prev->next = p->next;
			else sock->ctx->sock_list = p->next;
		}
		prev = p;
	}
	free(sock);
}

void lc_ctx_free(lc_ctx_t *ctx)
{
	if (ctx) {
		void *p, *h;
		p = ctx->sock_list;
		while (p) {
			h = p;
			p = ((lc_socket_t *)p)->next;
			lc_socket_close(h);
		}
		p = ctx->chan_list;
		while (p) {
			h = p;
			p = ((lc_channel_t *)p)->next;
			lc_channel_free(h);
		}
		if (ctx->sock >= 0) close(ctx->sock);
		free(ctx);
	}
}

int lc_socket_bind(lc_socket_t *sock, unsigned int ifx)
{
	if (setsockopt(sock->sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifx, sizeof ifx) == -1) {
		return -1;
	}
	sock->ifx = ifx;
	return 0;
}

lc_socket_t * lc_socket_new(lc_ctx_t *ctx)
{
	lc_socket_t *sock;
	int s, i, err = 0;

	sock = calloc(1, sizeof(lc_socket_t));
	if (!sock) return NULL;
	sock->ctx = ctx;
	sock->id = ++sock_id;
	sock->next = ctx->sock_list;
	ctx->sock_list = sock;
	s = socket(AF_INET6, SOCK_DGRAM, 0);
	if (s == -1) {
		err = errno;
		goto err_0;
	}
	sock->sock = s;
#ifdef IPV6_MULTICAST_ALL
	/* available in Linux 4.2 onwards */
	i = 0;
	if (setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_ALL, &i, sizeof i) == -1) {
		goto err_1;
	}
#endif
	i = 1;
	if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &i, sizeof i) == -1) {
		goto err_1;
	}
	i = DEFAULT_MULTICAST_LOOP;
	if (setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &i, sizeof i) == -1) {
		goto err_1;
	}
	i = DEFAULT_MULTICAST_HOPS;
	if (setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &i, sizeof i) == -1) {
		goto err_1;
	}
	return sock;
err_1:
	err = errno;
	close(s);
err_0:
	free(sock);
	errno = err;
	return NULL;
}

lc_ctx_t * lc_ctx_new(void)
{
	lc_ctx_t *ctx;

	if (!(ctx = calloc(1, sizeof(lc_ctx_t)))) return NULL; /* errno set by calloc */
	ctx->id = ++ctx_id;
	ctx->next = ctx_list;
	ctx_list = ctx;
	ctx->sock = -1;

	return ctx;
}
