#include "conf.h"
#include "slog.h"
#include "util.h"
#include "sockutil.h"
#include "jsonutil.h"

#include <assert.h>
#include <sys/socket.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MAX_CONF_SIZE 65536

static json_value *conf_parse(const char *filename)
{
	FILE *f = fopen(filename, "r");
	if (f == NULL) {
		LOGE_PERROR("unable to open config file");
		return NULL;
	}
	if (fseek(f, 0, SEEK_END)) {
		LOGE_PERROR("unable to seek config file");
		fclose(f);
		return NULL;
	}
	const long len = ftell(f);
	if (len < 0) {
		LOGE_PERROR("unable to tell config file length");
		fclose(f);
		return NULL;
	}
	if (len >= MAX_CONF_SIZE) {
		LOGE("config file is too large");
		fclose(f);
		return NULL;
	}
	if (fseek(f, 0, SEEK_SET)) {
		LOGE_PERROR("unable to seek config file");
		fclose(f);
		return NULL;
	}
	char *buf = util_malloc(len + 1);
	if (buf == NULL) {
		LOGF("conf_parse: out of memory");
		fclose(f);
		return NULL;
	}
	const size_t nread = fread(buf, sizeof(char), (size_t)len, f);
	fclose(f);
	if (nread != (size_t)len) {
		LOGE("unable to read the config file");
		util_free(buf);
		return NULL;
	}
	buf[nread] = '\0'; // end of string
	json_value *obj = parse_json(buf, nread);
	util_free(buf);
	if (obj == NULL) {
		LOGF("conf_parse: json parse failed");
		return NULL;
	}
	return obj;
}

static bool kcp_scope_cb(void *ud, const json_object_entry *entry)
{
	struct config *restrict conf = ud;
	const char *name = entry->name;
	const json_value *value = entry->value;
	if (strcmp(name, "mtu") == 0) {
		int mtu;
		if (!parse_int_json(&mtu, value)) {
			return false;
		}
		if (mtu < 300 || mtu > 1400) {
			LOGE("kcp.mtu out of range");
			return false;
		}
		conf->kcp_mtu = mtu;
		return true;
	}
	if (strcmp(name, "sndwnd") == 0) {
		return parse_int_json(&conf->kcp_sndwnd, value);
	}
	if (strcmp(name, "rcvwnd") == 0) {
		return parse_int_json(&conf->kcp_rcvwnd, value);
	}
	if (strcmp(name, "nodelay") == 0) {
		return parse_int_json(&conf->kcp_nodelay, value);
	}
	if (strcmp(name, "interval") == 0) {
		return parse_int_json(&conf->kcp_interval, value);
	}
	if (strcmp(name, "resend") == 0) {
		return parse_int_json(&conf->kcp_resend, value);
	}
	if (strcmp(name, "nc") == 0) {
		return parse_int_json(&conf->kcp_nc, value);
	}
	if (strcmp(name, "flush") == 0) {
		return parse_bool_json(&conf->kcp_flush, value);
	}
	LOGW_F("unknown config: \"kcp.%s\"", name);
	return true;
}

static bool tcp_scope_cb(void *ud, const json_object_entry *entry)
{
	struct config *restrict conf = ud;
	const char *name = entry->name;
	const json_value *value = entry->value;
	if (strcmp(name, "reuseport") == 0) {
		return parse_bool_json(&conf->tcp_reuseport, value);
	}
	if (strcmp(name, "keepalive") == 0) {
		return parse_bool_json(&conf->tcp_keepalive, value);
	}
	if (strcmp(name, "nodelay") == 0) {
		return parse_bool_json(&conf->tcp_nodelay, value);
	}
	if (strcmp(name, "sndbuf") == 0) {
		return parse_int_json(&conf->tcp_sndbuf, value);
	}
	if (strcmp(name, "rcvbuf") == 0) {
		return parse_int_json(&conf->tcp_rcvbuf, value);
	}
	LOGW_F("unknown config: \"tcp.%s\"", name);
	return true;
}

static bool udp_scope_cb(void *ud, const json_object_entry *entry)
{
	struct config *restrict conf = ud;
	const char *name = entry->name;
	const json_value *value = entry->value;
	if (strcmp(name, "sndbuf") == 0) {
		return parse_int_json(&conf->udp_sndbuf, value);
	}
	if (strcmp(name, "rcvbuf") == 0) {
		return parse_int_json(&conf->udp_rcvbuf, value);
	}
	LOGW_F("unknown config: \"udp.%s\"", name);
	return true;
}

static bool main_scope_cb(void *ud, const json_object_entry *entry)
{
	struct config *restrict conf = ud;
	const char *name = entry->name;
	const json_value *value = entry->value;
	if (strcmp(name, "kcp") == 0) {
		return walk_json_object(conf, value, kcp_scope_cb);
	}
	if (strcmp(name, "udp") == 0) {
		return walk_json_object(conf, value, udp_scope_cb);
	}
	if (strcmp(name, "tcp") == 0) {
		return walk_json_object(conf, value, tcp_scope_cb);
	}
	if (strcmp(name, "listen") == 0) {
		char *str = parse_string_json(value);
		return (conf->listen.str = str) != NULL;
	}
	if (strcmp(name, "connect") == 0) {
		char *str = parse_string_json(value);
		return (conf->connect.str = str) != NULL;
	}
	if (strcmp(name, "kcp_bind") == 0) {
		char *str = parse_string_json(value);
		return (conf->pkt_bind.str = str) != NULL;
	}
	if (strcmp(name, "kcp_connect") == 0) {
		char *str = parse_string_json(value);
		return (conf->pkt_connect.str = str) != NULL;
	}
#if WITH_CRYPTO
	if (strcmp(name, "method") == 0) {
		conf->method = parse_string_json(value);
		return conf->method != NULL;
	}
	if (strcmp(name, "password") == 0) {
		conf->password = parse_string_json(value);
		return conf->password != NULL;
	}
	if (strcmp(name, "psk") == 0) {
		conf->psk = parse_b64_json(value, &conf->psklen);
		return conf->psk != NULL;
	}
#endif /* WITH_CRYPTO */
#if WITH_OBFS
	if (strcmp(name, "obfs") == 0) {
		conf->obfs = parse_string_json(value);
		return conf->obfs != NULL;
	}
#endif /* WITH_OBFS */
	if (strcmp(name, "linger") == 0) {
		return parse_int_json(&conf->linger, value);
	}
	if (strcmp(name, "timeout") == 0) {
		return parse_int_json(&conf->timeout, value);
	}
	if (strcmp(name, "keepalive") == 0) {
		return parse_int_json(&conf->keepalive, value);
	}
	if (strcmp(name, "time_wait") == 0) {
		return parse_int_json(&conf->time_wait, value);
	}
	if (strcmp(name, "loglevel") == 0) {
		int l;
		if (!parse_int_json(&l, value)) {
			return false;
		}
		if (l < LOG_LEVEL_VERBOSE || l > LOG_LEVEL_SILENCE) {
			LOGE_F("log level out of range: %d - %d",
			       LOG_LEVEL_VERBOSE, LOG_LEVEL_SILENCE);
			return false;
		}
		conf->log_level = l;
		return true;
	}
	if (strcmp(name, "user") == 0) {
		conf->user = parse_string_json(value);
		return conf->user != NULL;
	}
	LOGW_F("unknown config: \"%s\"", name);
	return true;
}

const char *runmode_str(const int mode)
{
	static const char *str[] = {
		[MODE_SERVER] = "server",
		[MODE_CLIENT] = "client",
	};
	assert(mode >= 0);
	assert((size_t)mode < countof(str));
	return str[mode];
}

static bool splithostport(char *str, char **hostname, char **service)
{
	char *port = strrchr(str, ':');
	if (port == NULL) {
		util_free(str);
		return false;
	}
	*port = '\0';
	port++;

	char *host = str;
	if (host[0] == '\0') {
		/* default address */
		host = "::";
	} else if (host[0] == '[' && port[-2] == ']') {
		/* remove brackets */
		host++;
		port[-2] = '\0';
	}

	if (hostname != NULL) {
		*hostname = host;
	}
	if (service != NULL) {
		*service = port;
	}
	return true;
}

bool resolve_netaddr(struct netaddr *restrict addr, int flags)
{
	char *hostname = NULL;
	char *service = NULL;
	char *str = util_strdup(addr->str);
	if (str == NULL) {
		return NULL;
	}
	if (!splithostport(str, &hostname, &service)) {
		LOGE_F("failed splitting address: \"%s\"", addr->str);
		util_free(str);
		return false;
	}
	struct sockaddr *sa = resolve(hostname, service, flags);
	if (sa == NULL) {
		LOGE_F("failed resolving address: \"%s\"", addr->str);
		util_free(str);
		return false;
	}
	UTIL_SAFE_FREE(addr->sa);
	addr->sa = sa;
	if (LOGLEVEL(LOG_LEVEL_DEBUG)) {
		char addr_str[64];
		format_sa(sa, addr_str, sizeof(addr_str));
		LOGD_F("resolve: \"%s\" is %s", addr->str, addr_str);
	}
	util_free(str);
	return true;
}

static struct config conf_default(void)
{
	return (struct config){
		.kcp_mtu = 1400,
		.kcp_sndwnd = 512,
		.kcp_rcvwnd = 512,
		.kcp_nodelay = 0,
		.kcp_interval = 50,
		.kcp_resend = 3,
		.kcp_nc = 1,
		.kcp_flush = true,
		.password = NULL,
		.psk = NULL,
		.timeout = 600,
		.linger = 30,
		.keepalive = 25,
		.time_wait = 120,
		.tcp_reuseport = false,
		.tcp_keepalive = false,
		.tcp_nodelay = true,
		.log_level = LOG_LEVEL_INFO,
	};
}

static bool conf_check(struct config *restrict conf)
{
	/* 1. network address check */
	int mode = 0;
	if (conf->pkt_bind.str != NULL && conf->connect.str != NULL) {
		mode |= MODE_SERVER;
	}
	if (conf->listen.str != NULL && conf->pkt_connect.str != NULL) {
		mode |= MODE_CLIENT;
	}
	if (mode != MODE_SERVER && mode != MODE_CLIENT) {
		LOGF("config: no forward could be provided (are you missing some address field?)");
		return false;
	}
	conf->mode = mode;

	/* 2. crypto check */
	if (conf->psk != NULL && conf->password != NULL) {
		LOGF("config: psk and password cannot be specified at the same time");
		return false;
	}

	/* 3. range check */
	if (conf->kcp_interval < 10 || conf->kcp_interval > 500) {
		conf->kcp_interval = 50;
		LOGW_F("config: %s is out of range, using default: %d",
		       "kcp.interval", conf->kcp_interval);
	}
	if (conf->linger < 5 || conf->linger > 600) {
		conf->linger = 60;
		LOGW_F("config: %s is out of range, using default: %d",
		       "linger", conf->linger);
	}
	if (conf->timeout < 60 || conf->timeout > 86400) {
		conf->timeout = 600;
		LOGW_F("config: %s is out of range, using default: %d",
		       "timeout", conf->timeout);
	}
	if (conf->keepalive < 0 || conf->keepalive > 7200) {
		conf->keepalive = 25;
		LOGW_F("config: %s is out of range, using default: %d",
		       "keepalive", conf->timeout);
	}
	if (conf->time_wait < 5 || conf->time_wait > 3600 ||
	    conf->time_wait <= conf->linger) {
		conf->time_wait = conf->linger * 4;
		LOGW_F("config: %s is out of range, using default: %d",
		       "time_wait", conf->time_wait);
	}
	return true;
}

struct config *conf_read(const char *filename)
{
	struct config *conf = util_malloc(sizeof(struct config));
	if (conf == NULL) {
		return NULL;
	}
	*conf = conf_default();
	json_value *obj = conf_parse(filename);
	if (obj == NULL) {
		conf_free(conf);
		return NULL;
	}
	if (!walk_json_object(conf, obj, main_scope_cb)) {
		LOGE("invalid config file");
		conf_free(conf);
		json_value_free(obj);
		return NULL;
	}
	json_value_free(obj);
	if (!conf_check(conf)) {
		conf_free(conf);
		return NULL;
	}
	return conf;
}

static void netaddr_safe_free(struct netaddr *restrict addr)
{
	UTIL_SAFE_FREE(addr->str);
	UTIL_SAFE_FREE(addr->sa);
}

void conf_free(struct config *conf)
{
	netaddr_safe_free(&conf->listen);
	netaddr_safe_free(&conf->connect);
	netaddr_safe_free(&conf->pkt_bind);
	netaddr_safe_free(&conf->pkt_connect);
	UTIL_SAFE_FREE(conf->user);
#if WITH_CRYPTO
	UTIL_SAFE_FREE(conf->method);
	UTIL_SAFE_FREE(conf->password);
	UTIL_SAFE_FREE(conf->psk);
#endif
#if WITH_OBFS
	UTIL_SAFE_FREE(conf->obfs);
#endif
	util_free(conf);
}
