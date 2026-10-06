#ifndef SMIT_CA_QUEUE_H
#define SMIT_CA_QUEUE_H
#include <linux/dvb/ca.h>
/* Caller supplies serialization. No allocation or waiting on the RX path. */
#define SMIT_CA_QUEUE_DEPTH 16
struct smit_ca_queue {
	struct ca_msg messages[SMIT_CA_QUEUE_DEPTH];
	unsigned int head, count;
	bool invalidated, overflow;
};
static inline void smit_ca_queue_invalidate(struct smit_ca_queue *q)
{
	q->head = q->count = 0;
	q->invalidated = true;
	q->overflow = false;
}
static inline void smit_ca_queue_push(struct smit_ca_queue *q, const unsigned char *apdu,
				      unsigned int len)
{
	struct ca_msg *msg;
	if (q->count == SMIT_CA_QUEUE_DEPTH) {
		q->head = (q->head + 1) % SMIT_CA_QUEUE_DEPTH;
		q->count--;
		q->overflow = true;
	}
	msg = &q->messages[(q->head + q->count++) % SMIT_CA_QUEUE_DEPTH];
	memset(msg, 0, sizeof(*msg));
	msg->length = len;
	memcpy(msg->msg, apdu, len);
}
static inline int smit_ca_queue_pop(struct smit_ca_queue *q, struct ca_msg *msg)
{
	if (q->invalidated) {
		q->invalidated = false;
		return -ESTALE;
	}
	if (q->overflow) {
		q->overflow = false;
		return -EOVERFLOW;
	}
	if (!q->count)
		return -EAGAIN;
	*msg = q->messages[q->head];
	q->head = (q->head + 1) % SMIT_CA_QUEUE_DEPTH;
	q->count--;
	return 0;
}
#endif
