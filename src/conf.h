#ifndef CONF_H
#define CONF_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

struct config {
	struct sockaddr *addr_listen;
	struct sockaddr *addr_connect;
	struct sockaddr *addr_udp_bind, *addr_udp_connect;
	bool is_server;
	int udp_af;
	int kcp_mtu, kcp_sndwnd, kcp_rcvwnd;
	int kcp_nodelay, kcp_interval, kcp_resend, kcp_nc;
	char *password;
	unsigned char *psk;
	int timeout, linger, keepalive, time_wait;
	bool reuseport;
	int log_level;
};

struct config *conf_read(const char * /*file*/);
void conf_free(struct config * /*conf*/);

#endif /* CONF_H */
