#ifndef SMIT_CA_PMT_H
#define SMIT_CA_PMT_H
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
static inline int smit_ca_info_valid(const unsigned char *p, unsigned int len)
{
	unsigned int pos = 1;
	if (!len)
		return 1;
	if (p[0] < 1 || p[0] > 4)
		return 0;
	while (pos < len) {
		if (len - pos < 2 || p[pos + 1] > len - pos - 2)
			return 0;
		if (p[pos] == 9 && p[pos + 1] < 4)
			return 0;
		pos += p[pos + 1] + 2;
	}
	return pos == len;
}

static inline int smit_ca_pmt_valid(const unsigned char *p, unsigned int len)
{
	unsigned int pos, size;
	if (len < 10 || len > 250 || p[0] != 0x9f || p[1] != 0x80 || p[2] != 0x32)
		return 0;
	if (p[3] < 0x80) {
		if (p[3] + 4U != len)
			return 0;
		pos = 4;
	} else {
		if (p[3] != 0x81 || p[4] + 5U != len)
			return 0;
		pos = 5;
	}
	if (len - pos < 6 || p[pos] > 5)
		return 0;
	size = ((p[pos + 4] & 15) << 8) | p[pos + 5];
	pos += 6;
	if (size > len - pos || !smit_ca_info_valid(p + pos, size))
		return 0;
	pos += size;
	while (pos < len) {
		if (len - pos < 5)
			return 0;
		size = ((p[pos + 3] & 15) << 8) | p[pos + 4];
		pos += 5;
		if (size > len - pos || !smit_ca_info_valid(p + pos, size))
			return 0;
		pos += size;
	}
	return pos == len;
}

/* Android stop candidate: preserve program/version and ES identities, remove
 * every information loop. Device semantics remain unvalidated. Never use this
 * automatically; caller must explicitly opt in to the hardware experiment. */
static inline int smit_ca_stop_candidate(const unsigned char *p, unsigned int len,
					 unsigned char *out, unsigned int capacity)
{
	unsigned char body[250];
	unsigned int pos, used = 6, size, i, header;
	if (!smit_ca_pmt_valid(p, len))
		return -EINVAL;
	pos = p[3] < 128 ? 4 : 5;
	for (i = 0; i < 4; i++)
		body[i] = p[pos + i];
	body[4] = body[5] = 0;
	size = ((p[pos + 4] & 15) << 8) | p[pos + 5];
	pos += 6 + size;
	while (pos < len) {
		for (i = 0; i < 3; i++)
			body[used + i] = p[pos + i];
		body[used + 3] = body[used + 4] = 0;
		used += 5;
		size = ((p[pos + 3] & 15) << 8) | p[pos + 4];
		pos += 5 + size;
	}
	header = used < 128 ? 4 : 5;
	if (capacity < header + used)
		return -EMSGSIZE;
	out[0] = 0x9f;
	out[1] = 0x80;
	out[2] = 0x32;
	out[3] = used < 128 ? used : 0x81;
	if (header == 5)
		out[4] = used;
	for (i = 0; i < used; i++)
		out[header + i] = body[i];
	return header + used;
}

#endif
