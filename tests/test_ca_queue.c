#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "../driver/smit_ca_queue.h"
int main(void)
{
    struct smit_ca_queue q = {0};
    struct ca_msg msg;
    unsigned char apdu[] = {0x9f, 0x80, 0x33, 4, 0, 0, 1, 0x81};
    unsigned int i;
    assert(smit_ca_queue_pop(&q, &msg) == -EAGAIN);
    for (i = 0; i < SMIT_CA_QUEUE_DEPTH + 4; i++) {
        apdu[5] = i;
        smit_ca_queue_push(&q, apdu, sizeof(apdu));
    }
    assert(q.count == SMIT_CA_QUEUE_DEPTH);
    assert(smit_ca_queue_pop(&q, &msg) == -EOVERFLOW);
    for (i = 4; i < SMIT_CA_QUEUE_DEPTH + 4; i++) {
        assert(smit_ca_queue_pop(&q, &msg) == 0);
        assert(msg.length == sizeof(apdu) && msg.msg[5] == i);
    }
    assert(smit_ca_queue_pop(&q, &msg) == -EAGAIN);
    smit_ca_queue_push(&q, apdu, sizeof(apdu));
    smit_ca_queue_invalidate(&q);
    assert(smit_ca_queue_pop(&q, &msg) == -ESTALE);
    assert(smit_ca_queue_pop(&q, &msg) == -EAGAIN);
    smit_ca_queue_invalidate(&q);
    smit_ca_queue_push(&q, apdu, sizeof(apdu));
    assert(smit_ca_queue_pop(&q, &msg) == -ESTALE);
    assert(smit_ca_queue_pop(&q, &msg) == 0);
    puts("CA queue: FIFO, overflow reporting, stale generation purge and new generation delivery "
         "passed");
    return 0;
}
