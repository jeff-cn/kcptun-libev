#include "packet.h"
#include "event.h"
#include "event_impl.h"
#include "hashtable.h"
#include "kcp/ikcp.h"
#include "leakypool.h"
#include "proxy.h"
#include "aead.h"
#include "serialize.h"
#include "server.h"
#include "session.h"
#include "slog.h"
#include "util.h"
#include "sockutil.h"

#include <ev.h>

#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

const char tag_client[] = "kcptun-libev-client";
const size_t tag_client_size = sizeof(tag_client);
const char tag_server[] = "kcptun-libev-server";
const size_t tag_server_size = sizeof(tag_server);

static inline const unsigned char *
get_crypto_tag(const bool is_server, const bool is_seal, size_t *tag_size)
{
	const bool use_client_tag = is_server ^ is_seal;
	if (use_client_tag) {
		*tag_size = tag_client_size;
		return (unsigned char *)tag_client;
	}
	*tag_size = tag_server_size;
	return (unsigned char *)tag_server;
}

#if WITH_CRYPTO
static bool packet_open_inplace(
	struct packet *restrict p, unsigned char *data, size_t *restrict len,
	const size_t size)
{
	const size_t src_len = *len;
	UTIL_ASSERT(size >= src_len);
	struct aead *restrict crypto = p->crypto;
	const size_t nonce_size = crypto_nonce_size();
	const size_t overhead = aead_overhead(crypto);
	if (src_len <= nonce_size + overhead) {
		LOGD_F("packet too short: %zu", src_len);
		return false;
	}
	const unsigned char *nonce = data + src_len - nonce_size;
	unsigned char *saved_nonce = p->nonce_recv;
	if (saved_nonce == NULL) {
		saved_nonce = util_malloc(nonce_size);
		if (saved_nonce == NULL) {
			LOGW("packet_open: out of memory");
			return false;
		}
		p->nonce_recv = saved_nonce;
	} else if (!crypto_nonce_verify(saved_nonce, nonce)) {
		LOGD("weird nonce (attack?)");
		return false;
	}
	memcpy(saved_nonce, nonce, nonce_size);
	const size_t cipher_len = src_len - nonce_size;
	size_t tag_size;
	const unsigned char *tag =
		get_crypto_tag(p->is_server, false, &tag_size);
	const size_t dst_len = aead_open(
		crypto, data, size, nonce, data, cipher_len, tag, tag_size);
	if (dst_len + overhead + nonce_size != src_len) {
		LOGD("failed to open packet (wrong password?)");
		return false;
	}
	*len = dst_len;
	return true;
}

/* caller should ensure buffer is large enough */
static bool packet_seal_inplace(
	struct packet *restrict p, unsigned char *data, size_t *restrict len,
	const size_t size)
{
	struct aead *restrict crypto = p->crypto;
	const size_t src_len = *len;
	const size_t nonce_size = crypto_nonce_size();
	const size_t overhead = aead_overhead(crypto);
	UTIL_ASSERT(size >= src_len + overhead + nonce_size);
	unsigned char *nonce = p->nonce_send;
	crypto_nonce_next(nonce);
	const size_t dst_size = size - nonce_size;
	size_t tag_size;
	const unsigned char *tag =
		get_crypto_tag(p->is_server, true, &tag_size);
	size_t dst_len = aead_seal(
		crypto, data, dst_size, nonce, data, src_len, tag, tag_size);
	if (dst_len != src_len + overhead) {
		LOGE("failed to seal packet");
		return false;
	}
	memcpy(data + dst_len, nonce, nonce_size);
	*len = dst_len + nonce_size;
	return true;
}
#endif /* WITH_CRYPTO */

struct msgframe *msgframe_new(struct packet *p, struct sockaddr *sa)
{
	struct msgframe *restrict msg = pool_get(&p->msgpool);
	if (msg == NULL) {
		LOGE("msgframe: out of memory");
		return NULL;
	}
	msg->hdr = (struct msghdr){
		.msg_name = (struct sockaddr *)&msg->addr,
		.msg_namelen = sizeof(sockaddr_max_t),
		.msg_iov = &msg->iov,
		.msg_iovlen = 1,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_flags = 0,
	};
	msg->iov = (struct iovec){
		.iov_base = msg->buf,
		.iov_len = sizeof(msg->buf),
	};
	memset(&msg->addr, 0, sizeof(msg->addr));
	if (sa != NULL) {
		const socklen_t len = getsocklen(sa);
		memcpy(&msg->addr, sa, len);
		msg->hdr.msg_namelen = len;
	}
	return msg;
}

void msgframe_delete(struct packet *p, struct msgframe *msg)
{
	if (msg != NULL) {
		pool_put(&p->msgpool, msg);
	}
}

static inline struct session0_header ss0_header_read(const unsigned char *d)
{
	return (struct session0_header){
		.zero = read_uint32((const uint8_t *)d),
		.what = read_uint16((const uint8_t *)d + sizeof(uint32_t)),
	};
}

static inline void
ss0_header_write(unsigned char *d, struct session0_header header)
{
	write_uint32((uint8_t *)d, header.zero);
	write_uint16((uint8_t *)d + sizeof(uint32_t), header.what);
}

bool send_ss0(
	struct server *restrict s, struct sockaddr *sa, const uint16_t what,
	const unsigned char *b, const size_t n)
{
	struct packet *restrict p = s->udp.packets;
	if (p->mq_send_len >= MSG_QUEUE_SIZE) {
		LOGE("mq_send is full");
		return false;
	}
	struct msgframe *restrict msg = msgframe_new(p, sa);
	if (msg == NULL) {
		LOGE("out of memory");
		return false;
	}
	ss0_header_write(
		msg->buf, (struct session0_header){
				  .zero = 0,
				  .what = what,
			  });
	memcpy(msg->buf + SESSION0_HEADER_SIZE, b, n);
	msg->len = SESSION0_HEADER_SIZE + n;

	return packet_send(p, s, msg);
}

static void
ss0_keepalive(struct server *restrict s, struct msgframe *restrict msg)
{
	if (msg->len < SESSION0_HEADER_SIZE + sizeof(uint32_t)) {
		LOGW_F("short keepalive message: %zu bytes", msg->len);
		return;
	}
	const uint32_t tstamp = read_uint32(msg->buf + SESSION0_HEADER_SIZE);

	if (!s->conf->is_server) {
		/* client: print RTT */
		const uint32_t now_ms = tstamp2ms(ev_time());
		LOGI_F("roundtrip finished, RTT: %" PRIu32 " ms",
		       now_ms - tstamp);
		return;
	}

	/* server: send echo message */
	unsigned char b[sizeof(uint32_t)];
	write_uint32(b, tstamp);
	send_ss0(s, msg->hdr.msg_name, S0MSG_KEEPALIVE, b, sizeof(b));
}

static void session0(struct server *restrict s, struct msgframe *restrict msg)
{
	if (msg->len < SESSION0_HEADER_SIZE) {
		LOGW_F("short session 0 message: %zu bytes", msg->len);
		return;
	}
	struct session0_header header = ss0_header_read(msg->buf);
	switch (header.what) {
	case S0MSG_KEEPALIVE:
		ss0_keepalive(s, msg);
		break;
	default:
		LOGW_F("unknown session 0 message: %04" PRIX16, header.what);
		break;
	}
}

static void
packet_recv_one(struct server *restrict s, struct msgframe *restrict msg)
{
#if WITH_CRYPTO
	if (!packet_open_inplace(
		    s->udp.packets, msg->buf, &msg->len, MAX_PACKET_SIZE)) {
		return;
	}
#endif

	struct sockaddr *sa = (struct sockaddr *)&msg->addr;
	uint32_t conv = ikcp_getconv(msg->buf);
	if (conv == 0) {
		session0(s, msg);
		return;
	}
	hashkey_t sskey;
	conv_make_key(&sskey, sa, conv);
	struct session *restrict ss;
	if (!table_find(s->sessions, &sskey, (void **)&ss)) {
		if (!s->conf->is_server) {
			LOGW_F("session not found [%08" PRIX32 "]", conv);
			ss = session_new_dummy(s);
			if (ss != NULL) {
				table_set(s->sessions, &sskey, ss);
			}
			return;
		}
		/* server mode */
		ss = proxy_dial(s, sa, conv);
		if (ss == NULL) {
			return;
		}
	}
	if (ss->state == STATE_TIME_WAIT) {
		return;
	}

	ss->stats.udp_in += msg->len;
	int r = ikcp_input(ss->kcp, (const char *)msg->buf, msg->len);
	if (r < 0) {
		LOGW_F("ikcp_input: %d", r);
		return;
	}
	ss->kcp_checked = false;
}

void packet_recv(struct packet *restrict p, struct server *s)
{
	if (p->mq_recv_len == 0) {
		return;
	}
	for (size_t i = 0; i < p->mq_recv_len; i++) {
		struct msgframe *msg = p->mq_recv[i];
		packet_recv_one(s, msg);
		msgframe_delete(p, msg);
	}
	p->mq_recv_len = 0;
	kcp_notify_all(s);
}

bool packet_send(
	struct packet *restrict p, struct server *s,
	struct msgframe *restrict msg)
{
#if WITH_CRYPTO
	if (p->crypto != NULL) {
		UTIL_ASSERT(packet_seal_inplace(
			p, msg->buf, &msg->len, MAX_PACKET_SIZE));
	}
#endif

	if (p->mq_send_len >= MSG_QUEUE_SIZE) {
		LOGW_F("mq_send is full, %zu bytes discarded", msg->len);
		msgframe_delete(p, msg);
		return false;
	}
	msg->hdr.msg_namelen = getsocklen(msg->hdr.msg_name);
	msg->iov.iov_len = msg->len;
	p->mq_send[p->mq_send_len++] = msg;
	udp_notify_write(s);
	return true;
}

struct packet *packet_create(struct config *restrict cfg)
{
	struct packet *p = util_malloc(sizeof(struct packet));
	if (p == NULL) {
		return NULL;
	}
	*p = (struct packet){
		.msgpool = pool_create(100, sizeof(struct msgframe)),
		.is_server = cfg->is_server,
	};
	if (p->msgpool.pool == NULL) {
		packet_free(p);
		return NULL;
	}
#if WITH_CRYPTO
	if (cfg->psk) {
		p->crypto = aead_create(cfg->psk);
		UTIL_SAFE_FREE(cfg->psk);
	} else if (cfg->password) {
		p->crypto = aead_create_pw(cfg->password);
		UTIL_SAFE_FREE(cfg->password);
	}
	if (p->crypto != NULL) {
		p->nonce_send = util_malloc(crypto_nonce_size());
		if (p->nonce_send == NULL) {
			packet_free(p);
			return NULL;
		}
		crypto_nonce_init(p->nonce_send);
	} else {
		LOGW("data will not be encrypted");
	}

#else
	LOGW("data will not be encrypted");
#endif
	return p;
}

void packet_free(struct packet *restrict p)
{
	pool_free(&p->msgpool);
#if WITH_CRYPTO
	if (p->crypto != NULL) {
		aead_destroy(p->crypto);
		p->crypto = NULL;
		UTIL_SAFE_FREE(p->nonce_send);
		UTIL_SAFE_FREE(p->nonce_recv);
	}
#endif
	util_free(p);
}
