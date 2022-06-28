#include "aead.h"
#include "util.h"
#include "serialize.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if WITH_SODIUM

#include <sodium.h>

struct aead {
	unsigned char *key;
};

size_t crypto_nonce_size()
{
	return crypto_aead_chacha20poly1305_ietf_npubbytes();
}

void crypto_nonce_init(unsigned char *nonce)
{
	randombytes_buf(nonce, crypto_nonce_size());
}

const uint64_t nonce_magic = UINT64_C(999999937);

void crypto_nonce_next(unsigned char *nonce)
{
	UTIL_ASSERT(crypto_nonce_size() == sizeof(uint64_t) + sizeof(uint32_t));
	const uint64_t curr = read_uint64(nonce);
	uint64_t next = curr + nonce_magic;
	if (next < curr) { /* overflow */
		const uint64_t r0 = curr % nonce_magic;
		const uint64_t r1 = next % nonce_magic;
		next += nonce_magic - r1 + r0;
	}
	write_uint64(nonce, next);
	nonce += sizeof(uint64_t);
	write_uint32(nonce, rand32());
}

bool crypto_nonce_verify(const unsigned char *saved, const unsigned char *got)
{
	UTIL_ASSERT(crypto_nonce_size() == sizeof(uint64_t) + sizeof(uint32_t));
	const uint64_t r0 = read_uint64(saved) % nonce_magic;
	const uint64_t r1 = read_uint64(got) % nonce_magic;
	return r0 == r1;
}

size_t crypto_overhead()
{
	return crypto_aead_chacha20poly1305_ietf_abytes();
}

size_t crypto_key_size()
{
	return crypto_aead_chacha20poly1305_ietf_keybytes();
}

void crypto_gen_key(unsigned char *key)
{
	crypto_aead_chacha20poly1305_keygen(key);
}

static int kdf(unsigned char *restrict key, const char *restrict password)
{
	const char salt_str[] = "kcptun-libev";
	unsigned char salt[crypto_pwhash_argon2id_SALTBYTES];
	int r = crypto_generichash(
		salt, crypto_pwhash_argon2id_SALTBYTES,
		(unsigned char *)salt_str, strlen(salt_str), NULL, 0);
	if (r) {
		return r;
	}
	const size_t key_size = crypto_key_size();
	r = crypto_pwhash_argon2id(
		(unsigned char *)key, key_size, password, strlen(password),
		salt, crypto_pwhash_argon2id_OPSLIMIT_INTERACTIVE,
		crypto_pwhash_argon2id_MEMLIMIT_MIN,
		crypto_pwhash_argon2id_ALG_ARGON2ID13);
	return r;
}

void aead_keygen(unsigned char *k)
{
	crypto_aead_chacha20poly1305_ietf_keygen(k);
}

size_t aead_seal(
	struct aead *aead, unsigned char *dst, size_t dst_size,
	const unsigned char *nonce, const unsigned char *plain,
	size_t plain_size, const unsigned char *tag, size_t tag_size)
{
	UTIL_ASSERT(dst_size >= plain_size + crypto_overhead());
	unsigned long long r_len = dst_size;
	int r = crypto_aead_chacha20poly1305_ietf_encrypt(
		dst, &r_len, plain, plain_size, tag, tag_size, NULL, nonce,
		aead->key);
	if (r != 0) {
		LOGE_F("chacha20poly1305_ietf_encrypt: %d", r);
		return 0;
	}
	return r_len;
}

size_t aead_open(
	struct aead *aead, unsigned char *dst, size_t dst_size,
	const unsigned char *nonce, const unsigned char *cipher,
	size_t cipher_size, const unsigned char *tag, size_t tag_size)
{
	UTIL_ASSERT(dst_size + crypto_overhead() >= cipher_size);
	unsigned long long r_len = dst_size;
	int r = crypto_aead_chacha20poly1305_ietf_decrypt(
		dst, &r_len, NULL, cipher, cipher_size, tag, tag_size, nonce,
		aead->key);
	if (r != 0) {
		LOGE_F("chacha20poly1305_ietf_decrypt: %d", r);
		return 0;
	}
	return r_len;
}

void aead_init()
{
	const int ret = sodium_init();
	if (ret != 0) {
		LOGF_F("sodium_init failed: %d", ret);
		exit(EXIT_FAILURE);
	};
}

struct aead *aead_create_pw(char *password)
{
	if (password == NULL || strlen(password) == 0) {
		LOGI("no encryption enabled");
		return NULL;
	}
	struct aead *aead = util_malloc(sizeof(struct aead));
	if (aead == NULL) {
		return NULL;
	}
	const size_t key_size = crypto_key_size();
	unsigned char *key = sodium_malloc(key_size);
	if (key == NULL) {
		return NULL;
	}
	sodium_mlock(key, key_size);
	*aead = (struct aead){
		.key = key,
	};
	LOGI("key derivation...");
	int r = kdf(key, password);
	if (r) {
		LOGF_F("key derivation failed: %d", r);
	}
	memset(password, 0, strlen(password));
	return aead;
}

struct aead *aead_create(unsigned char *psk)
{
	if (psk == NULL) {
		LOGI("no encryption enabled");
		return NULL;
	}
	struct aead *aead = util_malloc(sizeof(struct aead));
	if (aead == NULL) {
		return NULL;
	}
	const size_t key_size = crypto_key_size();
	unsigned char *key = sodium_malloc(key_size);
	if (key == NULL) {
		return NULL;
	}
	sodium_mlock(key, key_size);
	*aead = (struct aead){
		.key = key,
	};
	LOGI("load psk...");
	memcpy(key, psk, key_size);
	memset(psk, 0, key_size);
	return aead;
}

void aead_destroy(struct aead *restrict aead)
{
	if (aead->key != NULL) {
		const size_t key_size = crypto_key_size();
		sodium_memzero(aead->key, key_size);
		sodium_munlock(aead->key, key_size);
		sodium_free(aead->key);
		aead->key = NULL;
	}
	util_free(aead);
}

#endif
