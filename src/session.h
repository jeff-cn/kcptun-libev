#ifndef SESSION_H
#define SESSION_H

#include "conf.h"
#include "serialize.h"
#include "hashtable.h"
#include "sockutil.h"
#include "util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

struct ev_io;
struct ev_loop;

struct server;

struct tlv_header {
	uint16_t msg;
	uint16_t len;
};

#define TLV_HEADER_SIZE (sizeof(struct tlv_header))

static inline struct tlv_header tlv_header_read(const unsigned char *d)
{
	return (struct tlv_header){
		.msg = read_uint16(d),
		.len = read_uint16(d + sizeof(uint16_t)),
	};
}

static inline void tlv_header_write(unsigned char *d, struct tlv_header header)
{
	write_uint16(d, header.msg);
	write_uint16(d + sizeof(uint16_t), header.len);
}

/* session messages */
enum session_messages {
	SMSG_DIAL = 0x0000,
	SMSG_PUSH = 0x0001,
	SMSG_EOF = 0x0002,
	SMSG_KEEPALIVE = 0x0003,

	SMSG_MAX,
};

enum session_state {
	STATE_HALFOPEN,
	STATE_CONNECT,
	STATE_CONNECTED,
	STATE_LINGER,
	STATE_TIME_WAIT,

	STATE_MAX,
};

struct link_stats {
	size_t tcp_in, tcp_out;
	size_t kcp_in, kcp_out;
	size_t udp_in, udp_out;
};

struct IKCPCB;

#define SESSION_BUF_SIZE 16384

struct session {
	bool is_accepted;
	int state;
	int tcp_fd;
	struct ev_io *w_read, *w_write;
	struct server *server;
	unsigned char *rbuf, *wbuf;
	size_t rbuf_len;
	size_t wbuf_len, wbuf_navail, wbuf_flush;
	sockaddr_max_t udp_remote;
	uint32_t conv;
	double last_send, last_recv;
	struct link_stats stats;
	struct IKCPCB *kcp;
	bool kcp_checked;
	uint32_t kcp_next;
};

struct session *
session_new(struct server *s, struct sockaddr *addr, uint32_t conv);
void session_free(struct session *ss);

void session_start(struct session *ss, int fd);
void session_shutdown(struct session *ss);
void session_on_msg(struct session *ss, struct tlv_header *hdr);

void session_close_all(struct hashtable *t);

#endif /* SESSION_H */
