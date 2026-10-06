#ifndef SMIT_CI_CODEC_H
#define SMIT_CI_CODEC_H
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
/* All lengths are bounded by the transport's currently supported 255 bytes. */
static inline int smit_apdu_body(const unsigned char *p, unsigned int len)
{
	unsigned int pos = 4, body;
	if (len < 4 || p[0] != 0x9f)
		return -EPROTO;
	body = p[3];
	if (body & 128) {
		if (body != 0x81 || len < 5)
			return -EPROTO;
		body = p[4];
		pos++;
	}
	return body == len - pos ? (int)pos : -EPROTO;
}

/* Return SAS payload offset; session comes from the current resource map. */
static inline int smit_sas_reply_session(const unsigned char *p, unsigned int len,
					 unsigned int session, unsigned int command)
{
	unsigned int pos = 9, body, message, payload;
	if (len < 9 || p[0] != 1 || p[1] != 0x90 || p[2] != 2 ||
	    (((unsigned int)p[3] << 8) | p[4]) != session || p[5] != 0x9f || p[6] != 0x9a ||
	    p[7] != 7)
		return -EPROTO;
	body = p[8];
	if (body & 128) {
		if (body != 0x81 || len < 10)
			return -EPROTO;
		body = p[9];
		pos++;
	}
	if (body < 7 || body != len - pos)
		return -EPROTO;
	message = (p[pos + 1] << 8) | p[pos + 2];
	payload = (p[pos + 5] << 8) | p[pos + 6];
	if (message + 3 != body || payload + 4 != message ||
	    (((unsigned int)p[pos + 3] << 8) | p[pos + 4]) != command)
		return -EPROTO;
	return pos + 7;
}
/* Compatibility for historical fixture tests, not used by the driver. */
static inline int smit_sas_reply(const unsigned char *p, unsigned int len, unsigned int command)
{
	return smit_sas_reply_session(p, len, 3, command);
}

#endif
