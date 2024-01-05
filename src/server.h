/* kcptun-libev (c) 2019-2024 He Xian <hexian000@outlook.com>
 * This code is licensed under MIT license (see LICENSE for details) */

#ifndef SERVER_H
#define SERVER_H

#include "conf.h"
#include "algo/hashtable.h"
#include "utils/buffer.h"
#include "session.h"
#include "sockutil.h"
#include "util.h"

#include <ev.h>
#include <netinet/in.h>

#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct listener {
	struct ev_io w_accept;
	struct ev_io w_accept_http;
	struct ev_timer w_timer;
	int fd;
	int fd_http;
};

struct pktconn {
	struct ev_io w_read, w_write;
	struct pktqueue *queue;
	int fd;
	int domain;
	bool connected;
	union sockaddr_max kcp_connect;
	ev_tstamp last_send_time;
	ev_tstamp last_recv_time;
	ev_tstamp inflight_ping;

	union sockaddr_max server_addr[2];
	union sockaddr_max rendezvous_server;
};

#define MAX_SESSIONS 65535

struct server {
	const struct config *conf;
	struct ev_loop *loop;
	struct listener listener;
	struct pktconn pkt;
	uint32_t m_conv;
	struct hashtable *sessions;
	struct {
		union sockaddr_max connect;

		double dial_timeout;
		double session_timeout, session_keepalive;
		double linger, time_wait;
		double keepalive, timeout;
		double ping_timeout;
	};
	struct {
		struct ev_timer w_kcp_update;
		struct ev_timer w_keepalive;
		struct ev_timer w_resolve;
		struct ev_timer w_timeout;
	};
	struct {
		struct link_stats stats, last_stats;
		ev_tstamp started;
		ev_tstamp last_stats_time;
		ev_tstamp last_resolve_time;
		clock_t clock, last_clock;
	};
};

struct server *server_new(struct ev_loop *loop, struct config *conf);
bool server_start(struct server *s);
void server_ping(struct server *s);
struct vbuffer *
server_stats_const(const struct server *s, struct vbuffer *buf, int level);
struct vbuffer *server_stats(struct server *s, struct vbuffer *buf, int level);
bool server_resolve(struct server *s);
void udp_rendezvous(struct server *s, uint16_t what);
void server_stop(struct server *s);
void server_free(struct server *s);

uint32_t conv_new(struct server *s, const struct sockaddr *sa);
size_t udp_overhead(const struct pktconn *udp);

#endif /* SERVER_H */
