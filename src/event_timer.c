#include "conf.h"
#include "event.h"
#include "event_impl.h"
#include "hashtable.h"
#include "packet.h"
#include "serialize.h"
#include "server.h"
#include "session.h"
#include "slog.h"
#include "sockutil.h"
#include "util.h"

#include <ev.h>
#include <stdint.h>
#include <sys/socket.h>

#include <math.h>

struct session_stats {
	size_t data[STATE_MAX];
	ev_tstamp now;
};

static bool print_session_iter(
	struct hashtable *t, const hashkey_t *key, void *value, void *user)
{
	UNUSED(t);
	UNUSED(key);
	struct session *restrict ss = value;
	struct session_stats *restrict stat = user;
	stat->data[ss->state]++;
	char addr_str[64];
	format_sa(
		(struct sockaddr *)&ss->udp_remote, addr_str, sizeof(addr_str));
	LOGD_F("session [%08" PRIX32
	       "] peer=%s state: %d age=%.0fs tcp(I/O)=%zu/%zu",
	       ss->conv, addr_str, ss->state, stat->now - ss->created,
	       ss->stats.tcp_in, ss->stats.tcp_out);
	return true;
}

static void print_debug_info(struct server *restrict s, const ev_tstamp now)
{
	static struct link_stats last_stats = { 0 };
	static double last_print_time = NAN;
	if (!isfinite(last_print_time)) {
		last_print_time = now;
		last_stats = s->stats;
		return;
	}
	if (now - last_print_time < 30.0) {
		return;
	}
	const size_t n_sessions = table_size(s->sessions);
	if (n_sessions > 0) {
		struct session_stats stats = (struct session_stats){
			.data = { 0 },
			.now = now,
		};
		table_iterate(s->sessions, &print_session_iter, &stats);
		LOGD_F("=== %zu sessions: %zu halfopen, %zu connected, %zu linger, %zu time_wait",
		       n_sessions,
		       stats.data[STATE_HALFOPEN] + stats.data[STATE_CONNECT],
		       stats.data[STATE_CONNECTED], stats.data[STATE_LINGER],
		       stats.data[STATE_TIME_WAIT]);
	}

	const double dt = now - last_print_time;
	struct link_stats dstats = (struct link_stats){
		.udp_in = s->stats.udp_in - last_stats.udp_in,
		.udp_out = s->stats.udp_out - last_stats.udp_out,
		.kcp_in = s->stats.kcp_in - last_stats.kcp_in,
		.kcp_out = s->stats.kcp_out - last_stats.kcp_out,
		.tcp_in = s->stats.tcp_in - last_stats.tcp_in,
		.tcp_out = s->stats.tcp_out - last_stats.tcp_out,
	};
	double udp_up, udp_down, tcp_up, tcp_down;
	udp_up = (dstats.udp_out >> 10u) / dt;
	udp_down = (dstats.udp_in >> 10u) / dt;
	tcp_up = (dstats.tcp_in >> 10u) / dt;
	tcp_down = (dstats.tcp_out >> 10u) / dt;
	LOGD_F("traffic(KiB/s) udp up/down: %.1f/%.1f; tcp up/down: %.1f/%.1f; efficiency: %.1f%%/%.1f%%",
	       udp_up, udp_down, tcp_up, tcp_down, tcp_up / udp_up * 100.0,
	       tcp_down / udp_down * 100.0);
	LOGD_F("total udp up/down: %zu/%zu; tcp up/down: %zu/%zu; efficiency: %.1f%%/%.1f%%",
	       s->stats.udp_out, s->stats.udp_in, s->stats.tcp_in,
	       s->stats.tcp_out, s->stats.tcp_in * 100.0 / s->stats.udp_out,
	       s->stats.tcp_out * 100.0 / s->stats.udp_in);
	last_print_time = now;
	last_stats = s->stats;
}

static bool
timeout_filt(struct hashtable *t, const hashkey_t *key, void *value, void *user)
{
	UNUSED(t);
	UNUSED(key);
	struct session *restrict ss = value;
	struct server *restrict s = ss->server;
	UTIL_ASSERT(ss->state < STATE_MAX);
	const ev_tstamp *restrict now = user;
	const double last_seen =
		ss->last_send > ss->last_recv ? ss->last_send : ss->last_recv;
	const double not_seen = *now - last_seen;
	switch (ss->state) {
	case STATE_HALFOPEN:
		if (not_seen > s->dial_timeout) {
			LOGW_F("session [%08" PRIX32 "] dial timed out",
			       ss->conv);
			session_shutdown(ss);
			kcp_close(ss);
			return true;
		}
		break;
	case STATE_CONNECT:
	case STATE_CONNECTED:
		if (not_seen > s->session_timeout) {
			LOGW_F("session [%08" PRIX32 "] state %d timed out",
			       ss->conv, ss->state);
			session_shutdown(ss);
			kcp_close(ss);
			return true;
		}
		if (!ss->is_accepted && not_seen > s->session_keepalive) {
			unsigned char buf[TLV_HEADER_SIZE];
			struct tlv_header header = (struct tlv_header){
				.msg = SMSG_KEEPALIVE,
				.len = TLV_HEADER_SIZE,
			};
			tlv_header_write(buf, header);
			(void)kcp_send(ss, buf, TLV_HEADER_SIZE);
			LOGD_F("session [%08" PRIX32 "] send: keepalive",
			       ss->conv);
		}
		break;
	case STATE_LINGER:
		if (not_seen > s->linger) {
			LOGD_F("session [%08" PRIX32 "] linger timed out",
			       ss->conv);
			ss->state = STATE_TIME_WAIT;
		}
		return true;
	case STATE_TIME_WAIT:
		if (not_seen > s->time_wait) {
			session_free(ss);
			return false;
		}
		return true;
	default:
		LOGW_F("unexpected session state: %d (bug?)", ss->state);
		UTIL_ASSERT(0);
		session_free(ss);
		return false;
	}
	return true;
}

static void timeout_check(struct server *restrict s, const ev_tstamp now)
{
	const double check_interval = 10.0;
	static ev_tstamp last_check = NAN;
	if (isfinite(last_check) && now - last_check < check_interval) {
		return;
	}
	last_check +=
		floor((now - last_check) / check_interval) * check_interval;
	const size_t n_sessions = table_size(s->sessions);
	if (n_sessions > 0) {
		table_filter(s->sessions, timeout_filt, (void *)&now);
	}
}

void timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents)
{
	CHECK_EV_ERROR(revents);

	struct server *restrict s = (struct server *)watcher->data;
	const ev_tstamp now = ev_now(loop);
	timeout_check(s, now);

	if (LOGLEVEL(LOG_LEVEL_DEBUG)) {
		print_debug_info(s, now);
	}

	if ((s->conf->mode & MODE_CLIENT) == 0) {
		return;
	}
	if (!(s->keepalive > 0.0)) {
		return;
	}
	const double timeout = s->keepalive * 3.0;
	if (now - s->udp.last_recv_time > timeout &&
	    now - s->last_resolve_time > timeout) {
		LOGD_F("remote not seen for %.0fs, try resolve addresses",
		       now - s->udp.last_recv_time);
		conf_resolve(s->conf);
#if WITH_CRYPTO
		noncegen_init(s->udp.packets->noncegen);
#endif
		s->last_resolve_time = now;
	}
	if (now - s->udp.last_send_time < s->keepalive) {
		return;
	}
	const uint32_t tstamp = tstamp2ms(ev_time());
	unsigned char b[sizeof(uint32_t)];
	write_uint32(b, tstamp);
	ss0_send(s, s->conf->udp_connect.sa, S0MSG_PING, b, sizeof(b));
	udp_notify_write(s);
}
