// main/couple_crypto.c —— 心跳报文 AES-128-CTR 加解密
#include "couple_app.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/aes.h"
#include <string.h>

static const char *TAG = "couple_crypto";

esp_err_t couple_crypto_init(void)
{
    return ESP_OK;
}

// AES-128-CTR 模式加解密（使用 ECB 模式手动实现，确保跨版本兼容）
static esp_err_t aes_128_ctr(const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *input, uint8_t len,
                             uint8_t *output)
{
    if (!key || !nonce || !input || !output || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    int ret = mbedtls_aes_setkey_enc(&ctx, key, 128);
    if (ret != 0) {
        mbedtls_aes_free(&ctx);
        ESP_LOGE(TAG, "AES 密钥设置失败: %d", ret);
        return ESP_FAIL;
    }

    // CTR 模式：12 字节 nonce + 4 字节计数器（大端）
    uint8_t ctr_block[16];
    uint8_t stream_block[16];

    memcpy(ctr_block, nonce, 12);
    memset(ctr_block + 12, 0, 4);

    for (uint8_t i = 0; i < len; i++) {
        uint8_t byte_idx = i % 16;
        if (byte_idx == 0) {
            // 加密当前计数器块
            ret = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, ctr_block, stream_block);
            if (ret != 0) {
                mbedtls_aes_free(&ctx);
                ESP_LOGE(TAG, "AES-ECB 加密失败: %d", ret);
                return ESP_FAIL;
            }
            // 递增计数器（大端，仅递增最后4字节）
            for (int j = 15; j >= 12; j--) {
                ctr_block[j]++;
                if (ctr_block[j] != 0) break;
            }
        }
        output[i] = input[i] ^ stream_block[byte_idx];
    }

    mbedtls_aes_free(&ctx);
    return ESP_OK;
}

esp_err_t couple_crypto_encrypt(const uint8_t *key, const uint8_t *nonce,
                                const uint8_t *plaintext, uint8_t len,
                                uint8_t *ciphertext)
{
    return aes_128_ctr(key, nonce, plaintext, len, ciphertext);
}

esp_err_t couple_crypto_decrypt(const uint8_t *key, const uint8_t *nonce,
                                const uint8_t *ciphertext, uint8_t len,
                                uint8_t *plaintext)
{
    return aes_128_ctr(key, nonce, ciphertext, len, plaintext);
}

// 生成随机数
void couple_crypto_random(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}

// CRC16-CCITT 校验
uint16_t couple_crypto_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}
