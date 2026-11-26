#ifndef EDGEHMI_OTA_PACKAGE_H
#define EDGEHMI_OTA_PACKAGE_H

#include "edgehmi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEHMI_EPK_MAGIC_LEN   4
#define EDGEHMI_EPK_HEADER_SIZE 64

typedef enum {
    EPK_TYPE_CONFIG   = 1,
    EPK_TYPE_FIRMWARE = 2
} edgehmi_epk_type_t;

typedef struct {
    char magic[4];            /* "EPK1" */
    uint8_t pkg_type;         /* edgehmi_epk_type_t */
    uint8_t reserved[3];
    char version[16];         /* null-terminated semver, e.g. "1.2.0" */
    uint32_t payload_len;     /* little-endian */
    uint8_t sha256[32];       /* SHA-256 of payload bytes */
    uint32_t header_crc32;    /* reserved / little-endian CRC32 of first 60 bytes */
} edgehmi_epk_header_t;

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    size_t datalen;
} edgehmi_sha256_ctx_t;

void edgehmi_sha256_init(edgehmi_sha256_ctx_t *ctx);
void edgehmi_sha256_update(edgehmi_sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void edgehmi_sha256_final(edgehmi_sha256_ctx_t *ctx, uint8_t out_hash[32]);
void edgehmi_sha256(const uint8_t *data, size_t len, uint8_t out_hash[32]);
void edgehmi_sha256_to_hex(const uint8_t hash[32], char out_hex[65]);

/* Parse & verify a complete in-memory .epk package (magic + length + SHA-256) */
edgehmi_err_t edgehmi_epk_verify(const uint8_t *buf,
                                 size_t total_len,
                                 edgehmi_epk_header_t *out_hdr,
                                 const uint8_t **out_payload);

/* Pack a payload into a valid .epk buffer (returns total bytes written, or 0 on error) */
size_t edgehmi_epk_pack(edgehmi_epk_type_t type,
                        const char *version,
                        const uint8_t *payload,
                        size_t payload_len,
                        uint8_t *out_buf,
                        size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* EDGEHMI_OTA_PACKAGE_H */
