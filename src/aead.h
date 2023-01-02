/* kcptun-libev (c) 2019-2023 He Xian <hexian000@outlook.com>
 * This code is licensed under MIT license (see LICENSE for details) */

#ifndef AEAD_H
#define AEAD_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* AEAD interface */
struct aead_impl;

enum noncegen_method {
	noncegen_counter,
	noncegen_random,
};

struct aead {
	const enum noncegen_method noncegen_method;
	const size_t nonce_size;
	const size_t overhead;
	const size_t key_size;
	struct aead_impl *impl;
};

struct aead *aead_create(const char *method);
void aead_password(struct aead *, char *password);
void aead_psk(struct aead *, unsigned char *psk);
void aead_free(struct aead *);

void aead_keygen(struct aead *, unsigned char *key);

size_t aead_seal(
	struct aead *, unsigned char *dst, size_t dst_size,
	const unsigned char *nonce, const unsigned char *plain,
	size_t plain_size, const unsigned char *tag, size_t tag_size);
size_t aead_open(
	struct aead *, unsigned char *dst, size_t dst_size,
	const unsigned char *nonce, const unsigned char *cipher,
	size_t cipher_size, const unsigned char *tag, size_t tag_size);

void aead_list_methods(void);

#endif /* AEAD_H */
