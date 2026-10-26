#ifndef QEMU_BSWAP_H
#define QEMU_BSWAP_H

static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

#define le32_to_cpu(v) ((uint32_t)(v))
#define cpu_to_le32(v) ((uint32_t)(v))
#define const_le32(v) ((uint32_t)(v))
#define le16_to_cpu(v) ((uint16_t)(v))
#define cpu_to_le16(v) ((uint16_t)(v))

static inline int ldub_p(const void *p) { return *(const uint8_t *)p; }
static inline int lduw_le_p(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline int lduw_be_p(const void *p) { uint16_t v; memcpy(&v, p, 2); return bswap16(v); }
static inline int ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return (int)v; }
static inline uint64_t ldq_le_p(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void stb_p(void *p, uint8_t v) { *(uint8_t *)p = v; }
static inline void stw_le_p(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void stl_le_p(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void stq_le_p(void *p, uint64_t v) { memcpy(p, &v, 8); }

#endif
