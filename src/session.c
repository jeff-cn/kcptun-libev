/* kcptun-libev (c) 2019-2024 He Xian <hexian000@outlook.com>
 * This code is licensed under MIT license (see LICENSE for details) */

#include "session.h"
#include "utils/buffer.h"
#include "utils/debug.h"
#include "utils/formats.h"
#include "utils/serialize.h"
#include "utils/slog.h"
#include "algo/hashtable.h"
#include "crypto.h"
#include "conf.h"
#include "event.h"
#include "server.h"
#include "pktqueue.h"
#include "sockutil.h"
#include "util.h"

#include "ikcp.h"

#include <ev.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

const char session_state_char[STATE_MAX] = {
	[STATE_INIT] = ' ',   [STATE_CONNECT] = '>',   [STATE_CONNECTED] = '-',
	[STATE_LINGER] = '.', [STATE_TIME_WAIT] = 'x',
};

static void kcp_log(const char *log, struct IKCPCB *kcp, void *user)
{
	UNUSED(kcp);
	struct session *restrict ss = user;
	LOGV_F("session [%08" PRIX32 "] kcp internal: %s", ss->conv, log);
}

static ikcpcb *
kcp_new(struct session *restrict ss, const struct config *restrict conf,
	uint32_t conv)
{
	ikcpcb *restrict kcp = ikcp_create(conv, ss);
	if (kcp == NULL) {
		return NULL;
	}
	ikcp_wndsize(kcp, conf->kcp_sndwnd, conf->kcp_rcvwnd);
	ikcp_setmtu(kcp, (int)ss->server->pkt.queue->mss);
	ikcp_nodelay(
		kcp, conf->kcp_nodelay, conf->kcp_interval, conf->kcp_resend,
		conf->kcp_nc);
	ikcp_setoutput(kcp, kcp_output);
	if (LOGLEVEL(VERBOSE)) {
		kcp->logmask = -1;
		kcp->writelog = kcp_log;
	}
	return kcp;
}

static void
ss_flush_cb(struct ev_loop *loop, struct ev_idle *watcher, int revents)
{
	UNUSED(revents);
	ev_idle_stop(loop, watcher);
	struct session *restrict ss = watcher->data;
	switch (ss->kcp_state) {
	case STATE_CONNECT:
	case STATE_CONNECTED:
	case STATE_LINGER:
		break;
	default:
		return;
	}
	ikcp_flush(ss->kcp);
	tcp_notify_recv(ss);
}

struct session *session_new(
	struct server *restrict s, const struct sockaddr *addr,
	const uint32_t conv)
{
	struct session *restrict ss =
		(struct session *)malloc(sizeof(struct session));
	if (ss == NULL) {
		return NULL;
	}
	const ev_tstamp now = ev_now(s->loop);
	SESSION_MAKEKEY(ss->key, addr, conv);
	ss->created = now;
	ss->last_send = TSTAMP_NIL;
	ss->last_recv = TSTAMP_NIL;
	ss->last_reset = TSTAMP_NIL;
	ss->tcp_state = STATE_INIT;
	ss->kcp_state = STATE_INIT;
	ev_io_init(&ss->w_socket, tcp_socket_cb, -1, EV_NONE);
	ss->w_socket.data = ss;
	ev_idle_init(&ss->w_flush, ss_flush_cb);
	ss->w_flush.data = ss;
	ss->server = s;
	memcpy(&ss->raddr, addr, getsocklen(addr));
	ss->conv = conv;
	ss->stats = (struct link_stats){ 0 };
	ss->kcp_flush = s->conf->kcp_flush;
	ss->is_accepted = false;
	ss->wbuf_next = ss->wbuf_flush = 0;
	ss->rbuf = VBUF_NEW(SESSION_BUF_SIZE);
	if (ss->rbuf == NULL) {
		session_free(ss);
		return NULL;
	}
	ss->wbuf = VBUF_NEW(SESSION_BUF_SIZE);
	if (ss->wbuf == NULL) {
		session_free(ss);
		return NULL;
	}
	ss->kcp = kcp_new(ss, s->conf, conv);
	if (ss->kcp == NULL) {
		session_free(ss);
		return NULL;
	}
	return ss;
}

void session_free(struct session *restrict ss)
{
	session_tcp_stop(ss);
	session_kcp_stop(ss);
	ev_idle_stop(ss->server->loop, &ss->w_flush);
	free(ss);
}

void session_start(struct session *restrict ss, const int fd)
{
	LOGD_F("session [%08" PRIX32 "] tcp: start, fd=%d", ss->conv, fd);
	/* Initialize and start watchers to transfer data */
	struct ev_loop *loop = ss->server->loop;
	struct ev_io *restrict w_socket = &ss->w_socket;
	ev_io_set(w_socket, fd, EV_READ | EV_WRITE);
	ev_io_start(loop, w_socket);

	const ev_tstamp now = ev_now(loop);
	const uint32_t now_ms = TSTAMP2MS(now);
	ikcp_update(ss->kcp, now_ms);
}

void session_tcp_stop(struct session *restrict ss)
{
	ss->tcp_state = STATE_TIME_WAIT;
	struct ev_io *restrict w_socket = &ss->w_socket;
	if (w_socket->fd == -1) {
		return;
	}
	LOGD_F("session [%08" PRIX32 "] tcp: stop, fd=%d", ss->conv,
	       w_socket->fd);
	ev_io_stop(ss->server->loop, w_socket);
	CLOSE_FD(ss->w_socket.fd);
	ev_io_set(w_socket, -1, EV_NONE);
}

void session_kcp_stop(struct session *restrict ss)
{
	ss->kcp_state = STATE_TIME_WAIT;
	if (ss->kcp != NULL) {
		ikcp_release(ss->kcp);
		ss->kcp = NULL;
	}
	ss->rbuf = VBUF_FREE(ss->rbuf);
	ss->wbuf = VBUF_FREE(ss->wbuf);
}

static void consume_wbuf(struct session *restrict ss, const size_t n)
{
	VBUF_CONSUME(ss->wbuf, n);
	ss->wbuf_flush = 0;
	ss->wbuf_next = 0;
}

static bool forward_dial(struct session *restrict ss, const struct sockaddr *sa)
{
	/* Create client socket */
	int fd = socket(sa->sa_family, SOCK_STREAM, 0);
	if (fd < 0) {
		const int err = errno;
		LOGE_F("socket: %s", strerror(err));
		return false;
	}
	if (!socket_set_nonblock(fd)) {
		const int err = errno;
		LOGE_F("fcntl: %s", strerror(err));
		CLOSE_FD(fd);
		return false;
	}
	{
		const struct config *restrict conf = ss->server->conf;
		socket_set_tcp(fd, conf->tcp_nodelay, conf->tcp_keepalive);
		socket_set_buffer(fd, conf->tcp_sndbuf, conf->tcp_rcvbuf);
	}

	/* Connect to address */
	if (connect(fd, sa, getsocklen(sa)) != 0) {
		const int err = errno;
		if (err != EINTR && err != EINPROGRESS) {
			LOGE_F("connect: %s", strerror(err));
			return false;
		}
		ss->tcp_state = STATE_CONNECT;
	} else {
		ss->tcp_state = STATE_CONNECTED;
	}

	if (LOGLEVEL(INFO)) {
		char addr_str[64];
		format_sa(sa, addr_str, sizeof(addr_str));
		LOG_F(INFO, "session [%08" PRIX32 "] tcp: connect %s", ss->conv,
		      addr_str);
	}
	session_start(ss, fd);
	return true;
}

static bool session_on_msg(
	struct session *restrict ss, const struct tlv_header *restrict hdr)
{
	switch (hdr->msg) {
	case SMSG_DIAL: {
		if (hdr->len != TLV_HEADER_SIZE) {
			break;
		}
		LOGD_F("session [%08" PRIX32 "] msg: dial", ss->conv);
		if (ss->tcp_state != STATE_INIT) {
			break;
		}
		if (!forward_dial(ss, &ss->server->connect.sa)) {
			break;
		}
		return true;
	}
	case SMSG_PUSH: {
		const size_t navail = (size_t)hdr->len - TLV_HEADER_SIZE;
		LOGV_F("session [%08" PRIX32 "] msg: push, %zu bytes", ss->conv,
		       navail);
		ss->wbuf_flush = TLV_HEADER_SIZE;
		tcp_notify_send(ss);
		return true;
	}
	case SMSG_EOF: {
		if (hdr->len != TLV_HEADER_SIZE) {
			break;
		}
		LOGI_F("session [%08" PRIX32 "] kcp: "
		       "connection closed by peer",
		       ss->conv);
		ss->kcp_state = STATE_LINGER;
		ss->tcp_state = STATE_LINGER;
		ss->wbuf_flush = ss->wbuf_next;
		tcp_notify_send(ss);
		return true;
	}
	case SMSG_KEEPALIVE: {
		if (hdr->len != TLV_HEADER_SIZE) {
			break;
		}
		LOGD_F("session [%08" PRIX32 "] msg: keepalive", ss->conv);
		if (ss->is_accepted) {
			if (!kcp_sendmsg(ss, SMSG_KEEPALIVE)) {
				return false;
			}
		}
		return true;
	}
	}
	LOGE_F("session [%08" PRIX32 "] msg: error "
	       "msg=%04" PRIX16 ", len=%04" PRIX16,
	       ss->conv, hdr->msg, hdr->len);
	return false;
}

static int ss_process(struct session *restrict ss)
{
	if (ss->wbuf_flush < ss->wbuf_next) {
		/* tcp flushing is in progress */
		return 0;
	}
	if (ss->wbuf->len < TLV_HEADER_SIZE) {
		/* no header available */
		return 0;
	}
	if (ss->wbuf_flush > 0) {
		/* tcp flushing is done */
		consume_wbuf(ss, ss->wbuf_flush);
	}
	const struct tlv_header hdr = tlv_header_read(ss->wbuf->data);
	if (hdr.len < TLV_HEADER_SIZE && hdr.len > TLV_MAX_LENGTH) {
		LOGE_F("unexpected message length: %" PRIu16, hdr.len);
		return -1;
	}
	if (hdr.msg < SMSG_MAX && ss->wbuf->len < hdr.len) {
		/* incomplete message */
		return 0;
	}
	ss->wbuf_next = hdr.len;
	if (!session_on_msg(ss, &hdr)) {
		/* malformed message */
		return -1;
	}
	if (ss->wbuf_flush == 0) {
		/* nothing to flush */
		consume_wbuf(ss, ss->wbuf_next);
	}
	return 1;
}

bool session_kcp_send(struct session *restrict ss)
{
	switch (ss->kcp_state) {
	case STATE_CONNECT:
	case STATE_CONNECTED:
		break;
	default:
		return false;
	}
	if (ss->rbuf->len == 0) {
		return true;
	}
	if (!kcp_push(ss)) {
		return false;
	}
	if (ss->kcp_flush >= 1) {
		session_kcp_flush(ss);
	}
	return true;
}

void session_kcp_close(struct session *restrict ss)
{
	switch (ss->kcp_state) {
	case STATE_CONNECT:
	case STATE_CONNECTED:
		break;
	default:
		kcp_reset(ss);
		return;
	}
	/* pass eof */
	if (!kcp_sendmsg(ss, SMSG_EOF)) {
		kcp_reset(ss);
		return;
	}
	LOGD_F("session [%08" PRIX32 "] kcp: close", ss->conv);
	ss->kcp_state = STATE_LINGER;
	if (ss->kcp_flush >= 1) {
		session_kcp_flush(ss);
	}
}

void session_read_cb(struct session *restrict ss)
{
	for (;;) {
		switch (ss->kcp_state) {
		case STATE_CONNECT:
		case STATE_CONNECTED:
			break;
		default:
			return;
		}
		switch (ss->tcp_state) {
		case STATE_INIT:
		case STATE_CONNECT:
		case STATE_CONNECTED:
			break;
		default:
			return;
		}
		kcp_recv(ss);
		const int ret = ss_process(ss);
		if (ret < 0) {
			session_tcp_stop(ss);
			session_kcp_close(ss);
			return;
		} else if (ret == 0) {
			return;
		}
	}
}

void session_kcp_flush(struct session *restrict ss)
{
	struct ev_idle *restrict w_flush = &ss->w_flush;
	if (ev_is_active(w_flush)) {
		return;
	}
	ev_idle_start(ss->server->loop, w_flush);
}

struct session0_header {
	uint32_t zero;
	uint16_t what;
};

#define SESSION0_HEADER_SIZE (sizeof(uint32_t) + sizeof(uint16_t))

static inline struct session0_header ss0_header_read(const unsigned char *d)
{
	return (struct session0_header){
		.zero = read_uint32(d),
		.what = read_uint16(d + sizeof(uint32_t)),
	};
}

static inline void
ss0_header_write(unsigned char *d, struct session0_header header)
{
	write_uint32(d, header.zero);
	write_uint16(d + sizeof(uint32_t), header.what);
}

size_t inetaddr_read(union sockaddr_max *addr, const void *b, const size_t n)
{
	const unsigned char *p = b;
	if (n < sizeof(uint8_t)) {
		return 0;
	}
	const enum inetaddr_type addrtype = read_uint8(p);
	p += sizeof(uint8_t);
	switch (addrtype) {
	case ATYP_INET:
		if (n < INETADDR_LENGTH) {
			return 0;
		}
		addr->in = (struct sockaddr_in){
			.sin_family = AF_INET,
		};
		memcpy(&addr->in.sin_addr, p, sizeof(struct in_addr));
		p += sizeof(struct in_addr);
		memcpy(&addr->in.sin_port, p, sizeof(in_port_t));
		return INETADDR_LENGTH;
	case ATYP_INET6:
		if (n < INET6ADDR_LENGTH) {
			return 0;
		}
		addr->in6 = (struct sockaddr_in6){
			.sin6_family = AF_INET6,
		};
		memcpy(&addr->in6.sin6_addr, p, sizeof(struct in6_addr));
		p += sizeof(struct in6_addr);
		memcpy(&addr->in6.sin6_port, p, sizeof(in_port_t));
		return INET6ADDR_LENGTH;
	default:
		break;
	}
	return 0;
}

size_t inetaddr_write(void *b, const size_t n, const struct sockaddr *sa)
{
	unsigned char *p = b;
	switch (sa->sa_family) {
	case AF_INET: {
		if (n < INETADDR_LENGTH) {
			return 0;
		}
		const struct sockaddr_in *restrict in =
			(const struct sockaddr_in *)sa;
		write_uint8(p, ATYP_INET);
		p += sizeof(uint8_t);
		memcpy(p, &in->sin_addr, sizeof(struct in_addr));
		p += sizeof(struct in_addr);
		memcpy(p, &in->sin_port, sizeof(in_port_t));
		return INETADDR_LENGTH;
	}
	case AF_INET6: {
		if (n < INET6ADDR_LENGTH) {
			return 0;
		}
		const struct sockaddr_in6 *restrict in6 =
			(const struct sockaddr_in6 *)sa;
		write_uint8(p, ATYP_INET6);
		p += sizeof(uint8_t);
		memcpy(p, &in6->sin6_addr, sizeof(struct in6_addr));
		p += sizeof(struct in6_addr);
		memcpy(p, &in6->sin6_port, sizeof(in_port_t));
		return INET6ADDR_LENGTH;
	}
	default:
		break;
	}
	return 0;
}

bool inetaddr_is_valid(const struct sockaddr *sa)
{
	switch (sa->sa_family) {
	case AF_INET: {
		const struct sockaddr_in *restrict in =
			(const struct sockaddr_in *)sa;
		return in->sin_addr.s_addr != INADDR_ANY && in->sin_port != 0;
	}
	case AF_INET6: {
		const struct sockaddr_in6 *restrict in6 =
			(const struct sockaddr_in6 *)sa;
		return !IN6_IS_ADDR_UNSPECIFIED(&in6->sin6_addr) &&
		       in6->sin6_port != 0;
	}
	default:
		break;
	}
	return false;
}

void ss0_reset(struct server *s, const struct sockaddr *sa, uint32_t conv)
{
	unsigned char b[sizeof(uint32_t)];
	write_uint32(b, conv);
	ss0_send(s, sa, S0MSG_RESET, b, sizeof(b));
}

bool ss0_send(
	struct server *restrict s, const struct sockaddr *sa,
	const uint16_t what, const unsigned char *b, const size_t n)
{
	struct msgframe *restrict msg = msgframe_new(s->pkt.queue);
	if (msg == NULL) {
		LOGOOM();
		return false;
	}
	memcpy(&msg->addr.sa, sa, getsocklen(sa));
	unsigned char *packet = msg->buf + msg->off;
	ss0_header_write(
		packet, (struct session0_header){
				.zero = 0,
				.what = what,
			});
	memcpy(packet + SESSION0_HEADER_SIZE, b, n);
	msg->len = SESSION0_HEADER_SIZE + n;
	return queue_send(s, msg);
}

static bool
ss0_on_ping(struct server *restrict s, struct msgframe *restrict msg)
{
	if (msg->len < SESSION0_HEADER_SIZE + sizeof(uint32_t)) {
		return false;
	}
	const unsigned char *msgbuf =
		msg->buf + msg->off + SESSION0_HEADER_SIZE;
	const uint32_t tstamp = read_uint32(msgbuf);
	/* send echo message */
	unsigned char b[sizeof(uint32_t)];
	write_uint32(b, tstamp);
	ss0_send(s, &msg->addr.sa, S0MSG_PONG, b, sizeof(b));
	return true;
}

static bool
ss0_on_pong(struct server *restrict s, struct msgframe *restrict msg)
{
	if (msg->len < SESSION0_HEADER_SIZE + sizeof(uint32_t)) {
		return false;
	}
	const unsigned char *msgbuf =
		msg->buf + msg->off + SESSION0_HEADER_SIZE;
	const uint32_t tstamp = read_uint32(msgbuf);
	/* calculate RTT & estimated bandwidth */
	const uint32_t now_ms = TSTAMP2MS(ev_time());
	const double rtt = (now_ms - tstamp) * 1e-3;
	const struct config *restrict conf = s->conf;
	const double rx = conf->kcp_rcvwnd * conf->kcp_mtu / rtt;
	const double tx = conf->kcp_sndwnd * conf->kcp_mtu / rtt;

	char bw_rx[16], bw_tx[16];
	format_iec_bytes(bw_rx, sizeof(bw_rx), rx);
	format_iec_bytes(bw_tx, sizeof(bw_tx), tx);

	LOGD_F("roundtrip finished in %" PRIu32 " ms, "
	       "capacity rx: %s/s, tx: %s/s",
	       now_ms - tstamp, bw_rx, bw_tx);
	s->pkt.inflight_ping = TSTAMP_NIL;

	if ((s->conf->mode & (MODE_RENDEZVOUS | MODE_CLIENT)) ==
	    (MODE_RENDEZVOUS | MODE_CLIENT)) {
		s->pkt.kcp_connect = msg->addr;
		s->pkt.connected = true;
	}
	return true;
}

static bool
ss0_on_reset(struct server *restrict s, struct msgframe *restrict msg)
{
	if (msg->len < SESSION0_HEADER_SIZE + sizeof(uint32_t)) {
		return false;
	}
	const unsigned char *msgbuf =
		msg->buf + msg->off + SESSION0_HEADER_SIZE;
	const uint32_t conv = read_uint32(msgbuf);
	unsigned char sskey[SESSION_KEY_SIZE];
	SESSION_MAKEKEY(sskey, &msg->addr.sa, conv);
	const struct hashkey hkey = {
		.len = sizeof(sskey),
		.data = sskey,
	};
	struct session *restrict ss;
	if (!table_find(s->sessions, hkey, (void **)&ss)) {
		return true;
	}
	if (ss->kcp_state == STATE_TIME_WAIT) {
		return true;
	}
	LOGI_F("session [%08" PRIX32 "] kcp: reset by peer", conv);
	session_tcp_stop(ss);
	session_kcp_stop(ss);
	return true;
}

static bool
ss0_on_listen(struct server *restrict s, struct msgframe *restrict msg)
{
	size_t msglen = msg->len;
	const unsigned char *msgbuf =
		msg->buf + msg->off + SESSION0_HEADER_SIZE;
	msglen -= SESSION0_HEADER_SIZE;
	size_t n = inetaddr_read(&s->pkt.server_addr[0], msgbuf, msglen);
	if (n == 0) {
		return false;
	}
	s->pkt.server_addr[1] = msg->addr;
	if (LOGLEVEL(DEBUG)) {
		char addr1_str[64], addr2_str[64];
		format_sa(
			&s->pkt.server_addr[0].sa, addr1_str,
			sizeof(addr1_str));
		format_sa(
			&s->pkt.server_addr[1].sa, addr2_str,
			sizeof(addr2_str));
		LOG_F(DEBUG, "rendezvous listen: (%s, %s)", addr1_str,
		      addr2_str);
	}
	s->pkt.listened = true;
	return true;
}

static bool
ss0_on_connect(struct server *restrict s, struct msgframe *restrict msg)
{
	size_t msglen = msg->len;
	const unsigned char *msgbuf =
		msg->buf + msg->off + SESSION0_HEADER_SIZE;
	msglen -= SESSION0_HEADER_SIZE;
	union sockaddr_max addr;
	size_t n = inetaddr_read(&addr, msgbuf, msglen);
	if (n == 0) {
		return false;
	}
	if (LOGLEVEL(INFO)) {
		char caddr1_str[64], caddr2_str[64];
		format_sa(&addr.sa, caddr1_str, sizeof(caddr1_str));
		format_sa(&msg->addr.sa, caddr2_str, sizeof(caddr2_str));
		char saddr1_str[64], saddr2_str[64];
		format_sa(
			&s->pkt.server_addr[0].sa, saddr1_str,
			sizeof(saddr1_str));
		format_sa(
			&s->pkt.server_addr[1].sa, saddr2_str,
			sizeof(saddr2_str));
		LOG_F(INFO, "rendezvous connect: (%s, %s) -> (%s, %s)",
		      caddr1_str, caddr2_str, saddr1_str, saddr2_str);
	}
	if (!s->pkt.listened) {
		LOGE("rendezvous connect: no server available");
		return true;
	}

	/* notify the server */
	unsigned char b[INET6ADDR_LENGTH + INET6ADDR_LENGTH];
	unsigned char *p = b;
	size_t len = sizeof(b);
	n = inetaddr_write(p, len, &addr.sa);
	if (n == 0) {
		return false;
	}
	p += n, len -= n;
	n = inetaddr_write(p, len, &msg->addr.sa);
	if (n == 0) {
		return false;
	}
	len -= n;
	n = sizeof(b) - len;
	ss0_send(s, &s->pkt.server_addr[1].sa, S0MSG_PUNCH, b, n);

	/* notify the client */
	p = b;
	len = sizeof(b);
	n = inetaddr_write(p, len, &s->pkt.server_addr[0].sa);
	if (n == 0) {
		return false;
	}
	p += n, len -= n;
	n = inetaddr_write(p, len, &s->pkt.server_addr[1].sa);
	if (n == 0) {
		return false;
	}
	len -= n;
	n = sizeof(b) - len;
	ss0_send(s, &msg->addr.sa, S0MSG_PUNCH, b, n);
	return true;
}

static bool
ss0_on_punch(struct server *restrict s, struct msgframe *restrict msg)
{
	size_t msglen = msg->len;
	const unsigned char *msgbuf =
		msg->buf + msg->off + SESSION0_HEADER_SIZE;
	msglen -= SESSION0_HEADER_SIZE;
	union sockaddr_max addr[2];
	size_t n = inetaddr_read(&addr[0], msgbuf, msglen);
	if (n == 0) {
		return false;
	}
	msgbuf += n, msglen -= n;
	n = inetaddr_read(&addr[1], msgbuf, msglen);
	if (n == 0) {
		return false;
	}
	if (LOGLEVEL(DEBUG)) {
		char addr1_str[64], addr2_str[64];
		format_sa(&addr[0].sa, addr1_str, sizeof(addr1_str));
		format_sa(&addr[1].sa, addr2_str, sizeof(addr2_str));
		LOG_F(DEBUG, "punch: (%s, %s)", addr1_str, addr2_str);
	}
	const ev_tstamp now = ev_now(s->loop);
	const uint32_t tstamp = TSTAMP2MS(now);
	unsigned char b[sizeof(uint32_t)];
	write_uint32(b, tstamp);
	if (inetaddr_is_valid(&addr[0].sa)) {
		ss0_send(s, &addr[0].sa, S0MSG_PING, b, sizeof(b));
	}
	if (inetaddr_is_valid(&addr[1].sa)) {
		ss0_send(s, &addr[1].sa, S0MSG_PING, b, sizeof(b));
	}
	return true;
}

static bool (*const ss0_handler[])(struct server *, struct msgframe *) = {
	[S0MSG_PING] = ss0_on_ping,	  [S0MSG_PONG] = ss0_on_pong,
	[S0MSG_RESET] = ss0_on_reset,	  [S0MSG_LISTEN] = ss0_on_listen,
	[S0MSG_CONNECT] = ss0_on_connect, [S0MSG_PUNCH] = ss0_on_punch,
};

void session0(struct server *restrict s, struct msgframe *restrict msg)
{
	if (msg->len < SESSION0_HEADER_SIZE) {
		LOGW_F("short session 0 message: %" PRIu16 " bytes", msg->len);
		return;
	}
	const unsigned char *packet = msg->buf + msg->off;
	struct session0_header header = ss0_header_read(packet);
	if (header.what < ARRAY_SIZE(ss0_handler)) {
		if (ss0_handler[header.what](s, msg)) {
			return;
		}
	}
	LOGW_F("invalid session 0 message: %04" PRIX16 ", len=%04" PRIX16,
	       header.what, msg->len - SESSION0_HEADER_SIZE);
}
