#ifndef __OTSERV_CRYPTO_H__
#define __OTSERV_CRYPTO_H__ 1

// RSA
bool RsaLoadPrivateKey(void);
bool RsaDecrypt(uint8_t *data, int len);

// XTEA
bool XteaEncrypt(const std::array<uint32_t, 4> &key, uint8_t *data, int len);
bool XteaDecrypt(const std::array<uint32_t, 4> &key, uint8_t *data, int len);

// CSRNG
void CryptoRand(uint8_t *buffer, int len);
uint8_t CryptoRandByte(void);

#endif //__OTSERV_CRYPTO_H__
