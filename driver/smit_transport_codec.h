#ifndef SMIT_TRANSPORT_CODEC_H
#define SMIT_TRANSPORT_CODEC_H
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
static inline int smit_wire_encode(unsigned char *out, unsigned int capacity, unsigned char request,
				   const unsigned char *data, unsigned int len)
{
	unsigned int i, offset = len < 128 ? 4 : 5;
	if (len > 255 || capacity < offset + len)
		return -EMSGSIZE;
	out[0] = 1;
	out[1] = 0;
	out[2] = request;
	out[3] = len < 128 ? len : 0x81;
	if (offset == 5)
		out[4] = len;
	for (i = 0; i < len; i++)
		out[offset + i] = data[i];
	return offset + len;
}
static inline int smit_wire_offset(const unsigned char *buf, int actual)
{
	if (actual < 4 || buf[0] != 1)
		return -EPROTO;
	if (buf[3] < 0x80)
		return 4;
	if (buf[3] == 0x81)
		return actual >= 5 ? 5 : -EMSGSIZE;
	return -EPROTO; /* Current receive buffer supports at most 255 payload bytes. */
}
static inline int smit_wire_length(const unsigned char *buf, int actual)
{
	int offset = smit_wire_offset(buf, actual);
	int length;
	if (offset < 0)
		return offset;
	length = buf[offset - 1];
	if (length > actual - offset)
		return -EMSGSIZE;
	return length;
}
/* Source-derived markers; both offsets must be within received data. */
static inline int smit_wire_complete(const unsigned char *buf, int actual)
{
	int len = smit_wire_length(buf, actual);
	int offset;
	if (len < 0)
		return 0;
	offset = smit_wire_offset(buf, actual);
	if (buf[2] == 0x80 && len >= 2 && buf[offset + 1] == 0x80)
		return 1;
	return actual >= len + offset + 4 && buf[len + offset] == 0x80 &&
	       buf[len + offset + 3] == 0x80;
}
#endif
