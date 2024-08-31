/* csnippets (c) 2019-2024 He Xian <hexian000@outlook.com>
 * This code is licensed under MIT license (see LICENSE for details) */

#include "url.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void hex(char *p, uint_fast8_t c)
{
	static const char hex[] = "0123456789ABCDEF";
	p[1] = hex[c & UINT8_C(0xF)];
	p[0] = hex[(c >> 4u) & UINT8_C(0xF)];
}

static int unhex(const unsigned char c)
{
	if (isdigit(c)) {
		return c - '0';
	}
	if ('A' <= c && c <= 'F') {
		return c - 'A' + 10;
	}
	if ('a' <= c && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

#define APPEND(str)                                                            \
	do {                                                                   \
		size_t n = strlen(str);                                        \
		if (buf_size < n) {                                            \
			n = buf_size;                                          \
		}                                                              \
		memcpy(buf, (str), n);                                         \
		buf += n;                                                      \
		buf_size -= n;                                                 \
	} while (0)

#define APPENDCH(ch)                                                           \
	do {                                                                   \
		if (buf_size >= 1) {                                           \
			*buf = ch;                                             \
			buf++;                                                 \
			buf_size--;                                            \
		}                                                              \
	} while (0)

#define APPENDESC(ch)                                                          \
	do {                                                                   \
		if (buf_size >= 3) {                                           \
			buf[0] = '%';                                          \
			hex(buf + 1, (uint_fast8_t)(ch));                      \
			buf += 3;                                              \
			buf_size -= 3;                                         \
		}                                                              \
	} while (0)

#define APPENDLIT(str)                                                         \
	do {                                                                   \
		size_t n = sizeof(str) - 1;                                    \
		if (buf_size < n) {                                            \
			n = buf_size;                                          \
		}                                                              \
		memcpy(buf, (str), n);                                         \
		buf += n;                                                      \
		buf_size -= n;                                                 \
	} while (0)

#define APPENDN(expr)                                                          \
	do {                                                                   \
		size_t n = (expr);                                             \
		buf += n;                                                      \
		buf_size -= n;                                                 \
	} while (0)

static size_t
escape(char *buf, size_t buf_size, const char *str, const char *allowed_symbols,
       const bool space)
{
	const size_t cap = buf_size;
	for (const char *p = str; *p != '\0'; ++p) {
		const unsigned char ch = *p;
		if (islower(ch) || isupper(ch) || isdigit(ch) ||
		    strchr(allowed_symbols, ch) != NULL) {
			APPENDCH(ch);
			continue;
		}
		if (space && ch == ' ') {
			APPENDCH('+');
			continue;
		}
		APPENDESC(ch);
	}
	return cap - buf_size;
}

static size_t escape_host(char *buf, size_t buf_size, const char *host)
{
	/* RFC 1738, RFC 2732 */
	return escape(buf, buf_size, host, "-_.~!$&'()*+,;=:[]<>\"", false);
}

static size_t escape_path(char *buf, size_t buf_size, const char *path)
{
	return escape(buf, buf_size, path, "-_.~$&+,/:;=@", false);
}

static size_t escape_userinfo(char *buf, size_t buf_size, const char *userinfo)
{
	return escape(buf, buf_size, userinfo, "-_.~$&+,;=", false);
}

static size_t escape_fragment(char *buf, size_t buf_size, const char *fragment)
{
	return escape(buf, buf_size, fragment, "-_.~$&+,/:;=?@!()*", false);
}

size_t
url_escape_userinfo(char *buf, size_t buf_size, char *username, char *password)
{
	size_t n = escape_userinfo(buf, buf_size, username);
	if (password == NULL) {
		return n;
	}
	if (n < buf_size) {
		buf[n++] = ':';
	}
	return n + escape_userinfo(buf, buf_size, password);
}

size_t url_escape_path(char *buf, size_t buf_size, const char *path)
{
	return escape(buf, buf_size, path, "-_.~$&+:=@", false);
}

size_t url_escape_query(char *buf, size_t buf_size, const char *query)
{
	return escape(buf, buf_size, query, "-_.~", true);
}

size_t url_build(char *buf, size_t buf_size, const struct url *url)
{
	const size_t cap = buf_size;

	/* [scheme:][//[userinfo@]host]/path[?query][#fragment] */
	if (url->scheme != NULL) {
		APPEND(url->scheme);
		APPENDCH(':');
	}

	if (url->defacto != NULL) {
		/* [scheme:]defacto */
		APPEND(url->defacto);
	} else {
		if (url->host != NULL) {
			APPENDLIT("//");
			if (url->userinfo != NULL) {
				APPENDN(escape_userinfo(
					buf, buf_size, url->userinfo));
				APPENDCH('@');
			}
			APPENDN(escape_host(buf, buf_size, url->host));
		}
		if (url->path != NULL) {
			if (url->path[0] != '/') {
				APPENDCH('/');
			}
			APPENDN(escape_path(buf, buf_size, url->path));
		}
	}

	if (url->query != NULL) {
		APPENDCH('?');
		APPEND(url->query);
	}
	if (url->fragment != NULL) {
		APPENDCH('#');
		APPENDN(escape_fragment(buf, buf_size, url->fragment));
	}

	return cap - buf_size;
}

static bool unescape(char *str, const bool space)
{
	char *w = str;
	for (char *r = str; *r != '\0'; r++) {
		char ch = *r;
		switch (ch) {
		case '%':
			switch (r[1]) {
			case '\0':
				return false;
			case '%':
				r++;
				break;
			default: {
				const int hi = unhex(r[1]);
				if (hi < 0) {
					return false;
				}
				const int lo = unhex(r[2]);
				if (lo < 0) {
					return false;
				}
				ch = (char)((hi << 4u) | lo);
				r += 2;
				break;
			}
			}
			break;
		case '+':
			if (space) {
				ch = ' ';
			}
			break;
		default:
			break;
		}
		*w++ = ch;
	}
	*w = '\0';
	return true;
}

static inline char *strlower(char *s)
{
	for (char *p = s; *p != '\0'; ++p) {
		*s = (unsigned char)tolower((unsigned char)*s);
	}
	return s;
}

bool url_parse(char *raw, struct url *restrict url)
{
	/* safety check */
	for (const char *p = raw; *p != '\0'; ++p) {
		if (*p < ' ' || *p == 0x7f) {
			return false;
		}
	}

	/* parse fragment */
	char *fragment = strchr(raw, '#');
	if (fragment != NULL) {
		*fragment = '\0';
		fragment++;
		if (!unescape(fragment, false)) {
			return false;
		}
	}
	*url = (struct url){
		.fragment = fragment,
	};

	if (*raw == '\0') {
		return false;
	}

	/* parse scheme */
	for (char *p = raw; *p != '\0'; ++p) {
		/* RFC 2396: Section 3.1 */
		if (isupper((unsigned char)*p) || islower((unsigned char)*p)) {
		} else if (
			isdigit((unsigned char)*p) || *p == '+' || *p == '-' ||
			*p == '.') {
			if (p == raw) {
				break;
			}
		} else if (*p == ':') {
			if (p == raw) {
				return false;
			}
			*p = '\0';
			url->scheme = strlower(raw);
			raw = p + 1;
			break;
		} else {
			break;
		}
	}

	/* parse query */
	url->query = strrchr(raw, '?');
	if (url->query != NULL) {
		*url->query = '\0';
		url->query++;
	}

	const bool has_1_slash = raw[0] == '/';
	const bool has_2_slashes = has_1_slash && raw[1] == '/';
	const bool has_3_slashes = has_2_slashes && raw[2] == '/';
	if (has_3_slashes) {
		raw += 3;
	} else if (has_2_slashes) {
		raw += 2;
		char *slash = strchr(raw, '/');
		if (slash != NULL) {
			*slash = '\0';
		}
		char *at = strrchr(raw, '@');
		if (at != NULL) {
			*at = '\0';
			url->userinfo = raw;
			raw = at + 1;
		}
		char *host = raw;
		if (!unescape(host, false)) {
			return false;
		}
		url->host = host;
		if (slash != NULL) {
			raw = slash + 1;
		} else {
			raw = NULL;
		}
	} else if (has_1_slash) {
		raw += 1;
	} else {
		url->defacto = raw;
		return true;
	}

	url->path = raw;
	return true;
}

bool url_path_segment(char **path, char **segment)
{
	char *s = *path;
	while (*s == '/') {
		s++;
	}
	char *next = strchr(s, '/');
	if (next != NULL) {
		*next = '\0';
		next++;
	}
	if (!unescape(s, false)) {
		return false;
	}
	*segment = s;
	*path = next;
	return true;
}

bool url_query_component(char **query, char **key, char **value)
{
	char *s = *query;
	char *next = strchr(s, '&');
	if (next != NULL) {
		*next = '\0';
		next++;
	}
	char *k = s;
	char *v = strchr(s, '=');
	if (v == NULL) {
		return false;
	}
	*v = '\0';
	v++;
	if (!unescape(k, true)) {
		return false;
	}
	if (!unescape(v, true)) {
		return false;
	}
	*key = k;
	*value = v;
	*query = next;
	return true;
}

bool url_unescape_userinfo(char *raw, char **username, char **password)
{
	const char valid_chars[] = "-._:~!$&\'()*+,;=%@'";
	char *colon = NULL;
	for (char *p = raw; *p != '\0'; ++p) {
		unsigned char c = (unsigned char)*p;
		/* RFC 3986: Section 3.2.1 */
		if (!islower(c) && !isupper(c) && !isdigit(c) &&
		    strchr(valid_chars, c) == NULL) {
			return false;
		}
		if (colon == NULL && c == ':') {
			colon = p;
		}
	}
	char *user = raw;
	char *pass = NULL;
	if (colon != NULL) {
		*colon = '\0';
		pass = colon + 1;
	}
	if (!unescape(user, false)) {
		return false;
	}
	if (pass != NULL && !unescape(pass, false)) {
		return false;
	}
	*username = user;
	*password = pass;
	return true;
}

bool url_unescape_path(char *str)
{
	return unescape(str, false);
}

bool url_unescape_query(char *str)
{
	return unescape(str, true);
}
