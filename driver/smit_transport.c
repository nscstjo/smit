#include "smit_internal.h"
#include "smit_transport_codec.h"

int smit_transport_exchange(struct smit_device *s, int txlen, unsigned long deadline)
{
	int ret, actual = 0;
	unsigned long remaining;
	unsigned int timeout;

	lockdep_assert_held(&s->lock);
	if (READ_ONCE(s->stopping))
		return -ENODEV;
	if (time_after_eq(jiffies, deadline))
		return -ETIMEDOUT;
	remaining = jiffies_to_msecs(deadline - jiffies);
	timeout = min_t(unsigned long, SMIT_USB_TIMEOUT_MS, remaining + 1);
	if (trace)
		print_hex_dump(KERN_DEBUG, "smit tx: ", DUMP_PREFIX_NONE, 16, 1, s->tx, txlen,
			       false);
	ret = usb_bulk_msg(s->udev, usb_sndbulkpipe(s->udev, SMIT_BULK_ENDPOINT_CMD_SEND), s->tx,
			   txlen, &actual, timeout);
	if (ret)
		return ret;
	if (actual != txlen)
		return -EREMOTEIO;
	if (READ_ONCE(s->stopping))
		return -ENODEV;
	if (time_after_eq(jiffies, deadline))
		return -ETIMEDOUT;
	timeout =
		min_t(unsigned long, SMIT_USB_TIMEOUT_MS, jiffies_to_msecs(deadline - jiffies) + 1);
	ret = usb_bulk_msg(s->udev, usb_rcvbulkpipe(s->udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE), s->rx,
			   sizeof(s->rx), &actual, timeout);
	if (trace && actual > 0)
		print_hex_dump(KERN_DEBUG, "smit rx: ", DUMP_PREFIX_NONE, 16, 1, s->rx, actual,
			       false);
	return ret ? ret : actual;
}

int smit_transport_request(struct smit_device *s, u8 req, const u8 *data, unsigned int len,
		   unsigned long deadline)
{
	int n, offset, size;
	if (len >= 128 && !experimental_long_tx)
		return -EMSGSIZE;
	n = smit_wire_encode(s->tx, sizeof(s->tx), req, data, len);
	if (n < 0)
		return n;
	n = smit_transport_exchange(s, n, deadline);
	if (n < 0)
		return n;
	size = smit_wire_length(s->rx, n);
	if (size < 0)
		return size;
	offset = smit_wire_offset(s->rx, n);
	if (n != offset + size && (n != offset + size + 4 || s->rx[offset + size] != 0x80 ||
				   (s->rx[n - 1] != 0 && s->rx[n - 1] != 0x80)))
		return -EPROTO;
	/* A completion flag describes queued data, never command success. */
	s->available = smit_wire_complete(s->rx, n);
	if (s->rx[2] == 0x80 && (size != 2 || s->rx[offset] != 1 ||
				 (s->rx[offset + 1] != 0 && s->rx[offset + 1] != 0x80)))
		return -EPROTO;
	if (req == 0x81) {
		memcpy(s->data, s->rx + offset, size);
		return size;
	}
	return 0;
}
