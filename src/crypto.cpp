#include "otpch.h"
#include "crypto.h"

#include <mutex>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>

// RSA
// =============================================================================
static std::mutex	g_RsaPrivateKeyMutex;
static RSA			*g_RsaPrivateKey = NULL;

static void DumpOpenSSLErrors(std::string_view where, std::string_view what){
	fmt::print("OpenSSL error(s) while executing {} at {}:\n", what, where);
	ERR_print_errors_cb(
		[](const char *str, size_t len, void *u) -> int {
			(void)u;

			// NOTE(fusion): Messages can have a trailing line feed.
			std::string_view sv = std::string_view(str, len);
			while(!sv.empty() && sv.back() == '\n'){
				sv.remove_suffix(1);
			}

			fmt::print(" - {}\n", sv);
			return 1;
		}, NULL);
}

bool RsaLoadPrivateKey(void){
	assert(g_RsaPrivateKey == NULL);

	BIO *bio = BIO_new_file("key.pem", "rb");
	if(!bio){
		fmt::print("RsaLoadPrivateKey: failed to open file \"key.pem\" for reading\n");
		return false;
	}

	g_RsaPrivateKey = PEM_read_bio_RSAPrivateKey(bio, NULL, NULL, NULL);
	BIO_free(bio);

	if(!g_RsaPrivateKey){
		DumpOpenSSLErrors("RsaLoadPrivateKey", "PEM_read_RSAPrivateKey");
		return false;
	}

	return true;
}

bool RsaDecrypt(uint8_t *data, int len){
	std::lock_guard lockGuard(g_RsaPrivateKeyMutex);

	if(len != RSA_size(g_RsaPrivateKey)){
		fmt::print("RsaDecrypt: invalid data length (expected: {}, got: {})",
				RSA_size(g_RsaPrivateKey), len);
		return false;
	}

	if(RSA_private_decrypt(len, data, data, g_RsaPrivateKey, RSA_NO_PADDING) == -1){
		DumpOpenSSLErrors("RsaDecrypt", "RSA_private_decrypt");
		return false;
	}

	return true;
}

// XTEA
// =============================================================================
// TODO(fusion): We're using XTEA in ECB mode, which means the encryption of a
// block doesn't depend on previous blocks. This makes it possible for us to use
// SIMD instrinsics to speed up encryption/decryption. This mode of operation is
// not as strong as the alternatives but we need to match the algorithm used by
// the client.

bool XteaEncrypt(const std::array<uint32_t, 4> &key, uint8_t *data, int len){
	if(len % 8 != 0){
		return false;
	}

	uint32_t v0, v1, delta, sum, i;
	while(len > 0){
		v0 = *(uint32_t*)(data + 0);
		v1 = *(uint32_t*)(data + 4);
		delta = 0x9E3779B9UL; sum = 0UL;
		for(i = 0; i < 32; ++i){
			v0 += ((v1<<4 ^ v1>>5) + v1) ^ (sum + key[sum & 3]);
			sum += delta;
			v1 += ((v0<<4 ^ v0>>5) + v0) ^ (sum + key[sum>>11 & 3]);
		}
		*(uint32_t*)(data + 0) = v0;
		*(uint32_t*)(data + 4) = v1;
		len -= 8; data += 8;
	}
	return true;
}

bool XteaDecrypt(const std::array<uint32_t, 4> &key, uint8_t *data, int len){
	if(len % 8 != 0){
		return false;
	}

	uint32_t v0, v1, delta, sum, i;
	while(len > 0){
		v0 = *(uint32_t*)(data + 0);
		v1 = *(uint32_t*)(data + 4);
		delta = 0x9E3779B9UL; sum = 0xC6EF3720UL;
		for(i = 0; i < 32; ++i){
			v1 -= ((v0<<4 ^ v0>>5) + v0) ^ (sum + key[sum>>11 & 3]);
			sum -= delta;
			v0 -= ((v1<<4 ^ v1>>5) + v1) ^ (sum + key[sum & 3]);
		}
		*(uint32_t*)(data + 0) = v0;
		*(uint32_t*)(data + 4) = v1;
		len -= 8; data += 8;
	}
	return true;
}

// CSRNG
// =============================================================================
void CryptoRand(uint8_t *buffer, int len){
	RAND_bytes(buffer, len);
}

uint8_t CryptoRandByte(void){
	uint8_t byte;
	RAND_bytes(&byte, 1);
	return byte;
}

uint8_t CryptoRandByte(void);

