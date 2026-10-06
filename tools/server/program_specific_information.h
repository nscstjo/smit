#ifndef DVB_SERVICE_PSI_H
#define DVB_SERVICE_PSI_H
#include <stdint.h>
#include <string.h>
#define SERVICE_MAX_ES 32
struct service_psi {
    uint8_t pmt[1024], capmt[250], pat[16];
    unsigned pmt_len, capmt_len, count, has_ca;
    uint16_t pid[SERVICE_MAX_ES], pcr, service, pmt_pid;
    uint8_t type[SERVICE_MAX_ES];
};
static uint32_t psi_crc(const uint8_t *p, unsigned n)
{
    uint32_t crc = 0xffffffff;
    unsigned i;
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (i = 0; i < 8; i++) crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
    }
    return crc;
}
static void psi_finish(uint8_t *p, unsigned n)
{
    uint32_t crc;
    unsigned len = n + 1;
    p[1] = 0xb0 | (len >> 8); p[2] = len;
    crc = psi_crc(p, n);
    p[n] = crc >> 24; p[n+1] = crc >> 16; p[n+2] = crc >> 8; p[n+3] = crc;
}
static int psi_valid(const uint8_t *p, unsigned n, unsigned table)
{
    return n >= 12 && n <= 1024 && p[0] == table && (p[1] & 0xf0) == 0xb0 &&
        (((p[1] & 15U) << 8) | p[2]) + 3 == n && (p[5] & 1) &&
        p[6] <= p[7] && !psi_crc(p, n);
}
/* PAT may have multiple sections; caller keeps listening until service found. */
static int psi_find_pmt(const uint8_t *p, unsigned n, unsigned service)
{
    unsigned pos;
    if (!psi_valid(p, n, 0) || (n - 12) % 4) return -1;
    for (pos = 8; pos < n - 4; pos += 4)
        if (((unsigned)p[pos] << 8 | p[pos+1]) == service) {
            unsigned pid = (p[pos+2] & 31U) << 8 | p[pos+3];
            return pid >= 0x20 && pid < 0x1fff ? (int)pid : -1;
        }
    return -1;
}
/* Validate every descriptor and remove CA descriptors only from output PSI. */
static int psi_descriptors(const uint8_t *p, unsigned n, uint8_t *out, unsigned *has_ca)
{
    unsigned pos = 0, used = 0;
    while (pos < n) {
        unsigned size;
        if (n - pos < 2 || (size = p[pos+1] + 2U) > n - pos) return -1;
        if (p[pos] == 9) {
            if (size < 6) return -1;
            *has_ca = 1;
        } else { memcpy(out + used, p + pos, size); used += size; }
        pos += size;
    }
    return (int)used;
}
static int psi_build(struct service_psi *s, const uint8_t *pat, unsigned pat_len,
                     const uint8_t *p, unsigned n, unsigned service)
{
    uint8_t body[1024];
    unsigned pos, clean = 12, used = 6, info, i;
    int filtered, pmt_pid = psi_find_pmt(pat, pat_len, service);
    memset(s, 0, sizeof(*s));
    if (pmt_pid < 0 || !psi_valid(p, n, 2) || n < 16 || p[6] || p[7] ||
        ((unsigned)p[3] << 8 | p[4]) != service || !service) return -1;
    s->service = service; s->pmt_pid = pmt_pid;
    s->pcr = (p[8] & 31U) << 8 | p[9];
    if (s->pcr < 0x20 || s->pcr == 0x1fff || s->pcr == s->pmt_pid) return -1;
    memcpy(s->pmt, p, 12);
    body[0] = 3; memcpy(body+1, p+3, 3);
    info = (p[10] & 15U) << 8 | p[11];
    if (info > n - 16) return -1;
    filtered = psi_descriptors(p+12, info, s->pmt+clean, &s->has_ca);
    if (filtered < 0) return -1;
    s->pmt[10] = 0xf0 | ((unsigned)filtered >> 8); s->pmt[11] = filtered;
    clean += filtered;
    body[4] = 0xf0 | ((info + !!info) >> 8); body[5] = info + !!info;
    if (info) { body[used++] = 1; memcpy(body+used, p+12, info); used += info; }
    pos = 12 + info;
    while (pos < n - 4) {
        unsigned pid, total;
        if (n - 4 - pos < 5 || s->count == SERVICE_MAX_ES) return -1;
        info = (p[pos+3] & 15U) << 8 | p[pos+4]; total = 5 + info;
        if (total > n - 4 - pos || used + total + !!info > sizeof(body)) return -1;
        pid = (p[pos+1] & 31U) << 8 | p[pos+2];
        if (pid < 0x20 || pid == 0x1fff || pid == s->pmt_pid) return -1;
        for (i = 0; i < s->count; i++) if (s->pid[i] == pid) return -1;
        s->type[s->count] = p[pos]; s->pid[s->count++] = pid;
        memcpy(s->pmt+clean, p+pos, 3);
        filtered = psi_descriptors(p+pos+5, info, s->pmt+clean+5, &s->has_ca);
        if (filtered < 0) return -1;
        s->pmt[clean+3] = 0xf0 | ((unsigned)filtered >> 8); s->pmt[clean+4] = filtered;
        clean += 5 + filtered;
        memcpy(body+used, p+pos, 3);
        body[used+3] = 0xf0 | ((info + !!info) >> 8); body[used+4] = info + !!info;
        used += 5;
        if (info) { body[used++] = 1; memcpy(body+used, p+pos+5, info); used += info; }
        pos += total;
    }
    if (!s->count) return -1;
    psi_finish(s->pmt, clean); s->pmt_len = clean + 4;
    /* Current driver long OUT framing is unverified. Fail before sending it. */
    if (s->has_ca && used + 4 > 122) return -2;
    if (s->has_ca) {
        memcpy(s->capmt, "\x9f\x80\x32", 3); s->capmt[3] = used;
        memcpy(s->capmt+4, body, used); s->capmt_len = used + 4;
    }
    memcpy(s->pat, pat, 8); s->pat[6] = s->pat[7] = 0;
    s->pat[8] = service >> 8; s->pat[9] = service;
    s->pat[10] = 0xe0 | ((unsigned)pmt_pid >> 8); s->pat[11] = pmt_pid;
    psi_finish(s->pat, 12);
    return 0;
}
#endif
