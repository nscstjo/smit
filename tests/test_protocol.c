#define SMIT_PROTOCOL_TEST
#include "../driver/smit_transport.c"
#include "../driver/smit_protocol.c"
#include "../driver/smit_worker.c"

struct frame {
    u8 bytes[255];
    unsigned int len;
};
static struct frame incoming[64];
static unsigned int head, tail, delivered, invalidations, sent, last_len;
static u8 last_tx[260], delivered_apdu[250];
static int transport_error;
static int control_result, control_calls;
static bool auto_card_reply;
static struct usb_device device;
static void sas(u16 id, u16 type, const u8 *p, unsigned int n);

static int usb_control_msg(struct usb_device *d, unsigned int pipe, u8 smit_transport_request, u8 type,
                           unsigned short value, unsigned short index, void *data,
                           unsigned short length, int timeout)
{
    assert(d == &device && pipe == 0 && smit_transport_request == 0xa0 && type == 0x40);
    assert(!value && !index && !data && !length && timeout == SMIT_USB_TIMEOUT_MS);
    control_calls++;
    return control_result;
}

void smit_ca_receive(struct smit_device *s, const u8 *p, unsigned int len)
{
    (void)s;
    assert(len <= sizeof(delivered_apdu));
    memcpy(delivered_apdu, p, len);
    delivered++;
}
void smit_ca_invalidate(struct smit_device *s)
{
    (void)s;
    invalidations++;
}

static int usb_bulk_msg(struct usb_device *d, unsigned int pipe, void *buf, int len, int *actual,
                        int timeout)
{
    u8 *p = buf;
    (void)d;
    assert(timeout > 0 && timeout <= SMIT_USB_TIMEOUT_MS);
    if (transport_error)
        return transport_error;
    if (pipe == SMIT_BULK_ENDPOINT_CMD_SEND) {
        assert((unsigned)len <= sizeof(last_tx));
        memcpy(last_tx, p, len);
        last_len = len;
        sent++;
        if (auto_card_reply && len >= 20 && p[2] == 0xa0 && p[9] == 0x9f && p[10] == 0x9a &&
            p[11] == 7 && p[16] == 0 && p[17] == SMIT_CMD_CARD_INFO)
            sas(get_be16(p + 7), 0x11, (const u8 *)"No Smart Card", 13);
        *actual = len;
        return 0;
    }
    assert(pipe == SMIT_BULK_ENDPOINT_CMD_RECEIVE);
    if (last_tx[0] == 0xfe) {
        memcpy(p, "\xff\0\x20\0", 4);
        *actual = 4;
    } else if (last_tx[2] == 0x81) {
        struct frame *f;
        assert(head < tail);
        f = &incoming[head++];
        *actual = smit_wire_encode(p, len, 0x81, f->bytes, f->len);
        if (head < tail) {
            memcpy(p + *actual, "\x80\0\0\x80", 4);
            *actual += 4;
        }
    } else {
        memcpy(p, "\1\0\x80\2\1\0", 6);
        p[5] = head < tail ? 0x80 : 0;
        *actual = 6;
    }
    return 0;
}
static void enqueue(const u8 *p, unsigned int n)
{
    assert(tail < 64 && n <= 255);
    memcpy(incoming[tail].bytes, p, n);
    incoming[tail++].len = n;
}
static void apdu(u16 id, const u8 *p, unsigned int n)
{
    u8 frame[255] = {1, 0x90, 2};
    put_be16(frame + 3, id);
    memcpy(frame + 5, p, n);
    enqueue(frame, n + 5);
}
static void sas(u16 id, u16 type, const u8 *p, unsigned int n)
{
    u8 a[200] = {0x9f, 0x9a, 7};
    assert(n < 120);
    a[3] = 7 + n;
    a[4] = 0xf3; /* Deliberately not the outbound counter. */
    put_be16(a + 5, n + 4);
    put_be16(a + 7, type);
    put_be16(a + 9, n);
    memcpy(a + 11, p, n);
    apdu(id, a, n + 11);
}
static void resource(u32 r)
{
    u8 p[7] = {1, 0x91, 4, r >> 24, r >> 16, r >> 8, r};
    enqueue(p, sizeof(p));
}
static void reset_fixture(struct smit_device *s)
{
    memset(s, 0, sizeof(*s));
    s->udev = &device;
    s->next_session = 1;
    head = tail = delivered = invalidations = sent = last_len = 0;
    transport_error = 0;
    control_result = control_calls = 0;
    experimental_ep0_reset = false;
    auto_card_reply = false;
    jiffies = 0;
}
static void sessions(struct smit_device *s)
{
    s->sessions[0] = (struct smit_session){SMIT_RESOURCE_SAS, 37, true};
    s->sessions[1] = (struct smit_session){SMIT_RESOURCE_CA, 42, true};
    s->sas_session = 37;
    s->ca_session = 42;
    s->initialized = true;
}
static void bootstrap(u16 first)
{
    const u8 profile_reply[] = {0x9f, 0x80, 0x11, 0};
    const u8 enquiry[] = {0x9f, 0x80, 0x10, 0};
    const u8 connected[] = {0x9f, 0x9a, 1, 9, 'S', 'M', 'i', 'T', 'Z', 'B', 'J', 'L', 0};
    const u8 info[] = {0x9f, 0x80, 0x31, 4, 0x4a, 2, 0x12, 0x34};
    resource(SMIT_RESOURCE_MANAGER);
    apdu(first, profile_reply, sizeof(profile_reply));
    apdu(first, enquiry, sizeof(enquiry));
    resource(SMIT_RESOURCE_APP);
    resource(SMIT_RESOURCE_SAS);
    apdu(first + 2, connected, sizeof(connected));
    resource(SMIT_RESOURCE_CA);
    apdu(first + 3, info, sizeof(info));
    resource(SMIT_RESOURCE_SAS);
}
int main(void)
{
    struct smit_device s;
    u8 reply[16], zero[4] = {0}, lock[16] = {0};
    const u8 info[] = {0x9f, 0x80, 0x31, 4, 0x4a, 2, 0x12, 0x34};
    const u8 card[] = {3};
    const u8 ca_reply[] = {0x9f, 0x80, 0x33, 4, 0, 115, 1, 0x81};
    struct smit_signal_status signal;
    int n;

    reset_fixture(&s);
    bootstrap(1);
    assert(smit_protocol_initialize(&s) == 0);
    assert(s.sas_session == 3 && s.ca_session == 4 && s.next_session == 6);
    assert(s.ca_info_len == 8 && delivered == 1 && head == tail);
    head = tail = 0;
    bootstrap(6);
    assert(smit_protocol_initialize(&s) == 0);
    assert(s.sas_session == 8 && s.ca_session == 9 && s.generation == 2);
    assert(s.next_session == 11); /* Session IDs never reused after timeout/reset. */

    reset_fixture(&s);
    resource(SMIT_RESOURCE_SAS);
    {
        const u8 connected[] = {0x9f, 0x9a, 1, 9, 'S', 'M', 'i', 'T', 'Z', 'B', 'J', 'L', 0};
        apdu(1, connected, sizeof(connected));
    }
    assert(smit_protocol_initialize(&s) == 0 && !s.ca_session && !s.ca_info_len);
    /* Missing card/CA_INFO/CA resource must not prevent frontend registration. */

    reset_fixture(&s);
    sessions(&s);
    apdu(42, info, sizeof(info));
    apdu(42, ca_reply, sizeof(ca_reply));
    sas(37, 0x1005, (const u8 *)"log", 3);
    sas(3, 6, lock, 16); /* Retired session must not satisfy current smit_protocol_status. */
    sas(37, 6, lock, 16);
    n = smit_protocol_sas_command(&s, 5, (const u8 *)"GSTA", 4, reply, sizeof(reply));
    assert(n == 16 && delivered == 2 && head == tail && !s.faulted);

    reset_fixture(&s);
    sessions(&s);
    sas(37, 0x100c, card, sizeof(card));
    sas(37, 6, lock, 16);
    assert(smit_protocol_sas_command(&s, 5, (const u8 *)"GSTA", 4, reply, sizeof(reply)) == 16);
    assert(!s.faulted && s.initialized && s.ca_info_len == 0 && invalidations == 1);
    assert(s.sas_session == 37 && s.card_refresh && s.ca_refresh);
    /* Card events smit_protocol_invalidate CA state, not a still-working CI session. */

    reset_fixture(&s);
    sessions(&s);
    assert(smit_protocol_sas_command(&s, 5, (const u8 *)"GSTA", 4, reply, sizeof(reply)) == -ETIMEDOUT);
    assert(jiffies <= SMIT_COMMAND_TIMEOUT_MS + 10 && s.faulted);
    n = sent;
    {
        struct command_job job = {.s = &s, .op = OP_STATUS};
        command_worker(&job.work);
        assert(job.result == -EAGAIN && sent == (unsigned)n);
    }

    reset_fixture(&s);
    sessions(&s);
    transport_error = -ENODEV;
    assert(smit_protocol_sas_command(&s, 5, (const u8 *)"GSTA", 4, reply, sizeof(reply)) == -ENODEV);
    assert(s.faulted && !s.tuned);

    reset_fixture(&s);
    sessions(&s);
    sas(37, 4, zero, 3);
    assert(smit_protocol_tune(&s, 147000000, 6875000) == -EPROTO && !s.tuned);
    reset_fixture(&s);
    sessions(&s);
    zero[3] = 7;
    sas(37, 4, zero, 4);
    assert(smit_protocol_tune(&s, 147000000, 6875000) == -EREMOTEIO && !s.tuned);
    zero[3] = 0;
    reset_fixture(&s);
    sessions(&s);
    sas(37, 4, zero, 4);
    assert(smit_protocol_tune(&s, 147000000, 6875000) == 0 && s.tuned);
    put_le32(lock, 147000);
    put_le32(lock + 4, 6875);
    lock[13] = 1;
    sas(37, 6, lock, 16);
    assert(smit_protocol_status(&s, &signal) == 0 && signal.locked);
    lock[13] = 0;
    sas(37, 6, lock, 16);
    assert(smit_protocol_status(&s, &signal) == 0 && !signal.locked && s.tuned); /* Cable loss isn't USB failure. */

    s.recovery_attempts = 2;
    sas(37, 6, lock, 16);
    assert(smit_protocol_status(&s, &signal) == 0 && s.recovery_attempts == 2);
    /* One good smit_protocol_status between repeated faults must not reset the retry cap. */
    jiffies = s.healthy_since + SMIT_RECOVERY_STABLE_MS;
    s.card_check_at = jiffies + SMIT_CARD_CHECK_MS;
    poll_worker(&s.poll_work.work);
    assert(!s.faulted && s.recovery_attempts == 0);

    reset_fixture(&s);
    s.faulted = true;
    transport_error = -EIO;
    for (n = 0; n < SMIT_RECOVERY_LIMIT; n++) {
        jiffies = s.recovery_at;
        poll_worker(&s.poll_work.work);
    }
    assert(s.recovery_attempts == SMIT_RECOVERY_LIMIT);
    assert(s.generation == SMIT_RECOVERY_LIMIT);
    jiffies = s.recovery_at;
    poll_worker(&s.poll_work.work);
    assert(s.generation == SMIT_RECOVERY_LIMIT); /* No fourth handshake. */

    reset_fixture(&s);
    sessions(&s);
    {
        const u8 malformed[] = {0x9f, 0x80, 0x31, 1, 0x4a};
        apdu(42, malformed, sizeof(malformed));
        assert(smit_protocol_pump(&s, 0, 0, 1000) == -EPROTO && !delivered);
    }
    reset_fixture(&s);
    sessions(&s);
    {
        const u8 close_session[] = {1, 0x95, 2, 0, 37};
        enqueue(close_session, sizeof(close_session));
        assert(smit_protocol_pump(&s, 0, 0, 1000) == -ECONNRESET);
        assert(last_tx[5] == 0x96);
    }

    reset_fixture(&s);
    sessions(&s);
    {
        struct command_job job = {.s = &s, .op = OP_RESET};
        command_worker(&job.work);
        assert(job.result == -EOPNOTSUPP && sent == 0);
    }
    s.stopping = true;
    assert(smit_transport_request(&s, 0xa0, zero, 1, 1000) == -ENODEV && sent == 0);

    reset_fixture(&s);
    s.faulted = true;
    {
        struct command_job job = {.s = &s, .op = OP_SUSPEND};
        command_worker(&job.work);
        assert(job.result == 0 && s.suspended && !sent);
    }

    reset_fixture(&s);
    sessions(&s);
    experimental_power = true;
    sas(37, 0xa, zero, 4);
    {
        struct command_job job = {.s = &s, .op = OP_SUSPEND};
        command_worker(&job.work);
        assert(job.result == 0 && s.suspended);
    }
    head = tail = 0;
    sas(37, 0xc, zero, 4);
    {
        struct command_job job = {.s = &s, .op = OP_RESUME};
        command_worker(&job.work);
        assert(job.result == 0 && !s.suspended && s.initialized && s.sas_session == 37);
        assert(s.ca_refresh && s.card_refresh && s.generation == 0);
    }
    head = tail = 0;
    sas(37, 2, zero, 4);
    {
        struct command_job job = {.s = &s, .op = OP_RESET};
        command_worker(&job.work);
        assert(job.result == 0 && s.initialized && s.sas_session == 37);
        assert(!s.tuned && s.ca_refresh && s.card_refresh && s.generation == 0);
    }
    reset_fixture(&s);
    sessions(&s);
    zero[3] = 1;
    sas(37, 0xa, zero, 4);
    {
        struct command_job job = {.s = &s, .op = OP_SUSPEND};
        command_worker(&job.work);
        assert(job.result == -EREMOTEIO && !s.suspended);
    }
    zero[3] = 0;
    experimental_power = false;

    reset_fixture(&s);
    {
        u8 large[128] = {0};
        assert(smit_transport_request(&s, 0xa0, large, sizeof(large), 1000) == -EMSGSIZE && !sent);
        experimental_long_tx = true;
        assert(smit_transport_request(&s, 0xa0, large, sizeof(large), 1000) == 0 && last_len == 133);
        assert(last_tx[3] == 0x81 && last_tx[4] == 128);
        experimental_long_tx = false;
    }
    reset_fixture(&s);
    sessions(&s);
    auto_card_reply = true;
    poll_worker(&s.poll_work.work);
    assert(s.card_state == SMIT_CARD_ABSENT && s.card_info_valid && !s.faulted);
    n = sent;
    jiffies = s.card_check_at - 1;
    poll_worker(&s.poll_work.work);
    assert(sent == (unsigned)n + 1); /* Poll only, no premature GCIN. */
    jiffies = s.card_check_at;
    poll_worker(&s.poll_work.work);
    assert(!s.faulted && invalidations == 0); /* Unchanged text cannot force reselection. */
    auto_card_reply = false;
    sas(37, 0x11, (const u8 *)"unrecognized", 12);
    assert(smit_protocol_refresh_card(&s) == 0 && s.card_state == SMIT_CARD_UNKNOWN);
    assert(invalidations == 1 && s.ca_refresh && s.initialized);
    /* Do not keep an old 'absent' state after an unrecognized reply. */
    sas(37, SMIT_EVENT_CARD, card, sizeof(card));
    sas(37, 0x11, (const u8 *)"stale", 5);
    assert(smit_protocol_refresh_card(&s) == 0 && s.card_refresh);
    assert(s.card_info_len == 12); /* Interleaved notification discards the query result. */

    reset_fixture(&s);
    sessions(&s);
    experimental_ep0_reset = true;
    control_result = -EPROTO; /* Reboot may terminate its own control transfer. */
    {
        struct command_job job = {.s = &s, .op = OP_RESET};
        command_worker(&job.work);
        assert(job.result == -EPROTO && control_calls == 1 && s.reset_pending);
        assert(!s.initialized && !s.tuned);
        command_worker(&job.work);
        assert(job.result == -EAGAIN && control_calls == 1);
        job.op = OP_SUSPEND;
        command_worker(&job.work);
        assert(job.result == 0 && s.suspended);
        job.op = OP_RESUME;
        command_worker(&job.work);
        assert(job.result == -EAGAIN && !s.suspended);
    }
    jiffies = s.reset_deadline;
    poll_worker(&s.poll_work.work);
    assert(s.faulted && !s.reset_pending && s.recovery_attempts == SMIT_RECOVERY_LIMIT);
    jiffies += 60000;
    poll_worker(&s.poll_work.work);
    assert(control_calls == 1 && sent == 0); /* No reboot or handshake loop. */

    puts("protocol: bootstrap/rebuild, interleaving, stale session, card invalidation, timeout, "
         "disconnect, result and power gates passed");
    return 0;
}
