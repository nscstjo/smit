#include "smit_internal.h"
#include "smit_ci_codec.h"

void smit_protocol_invalidate(struct smit_device *s)
{
	s->initialized = false;
	s->tuned = false;
	s->ca_info_len = 0;
	s->card_event_valid = false;
	s->ca_refresh = false;
	s->card_refresh = false;
	s->card_info[0] = 0;
	s->card_info_valid = false;
	s->card_state = SMIT_CARD_UNKNOWN;
	smit_ca_invalidate(s);
}

void smit_protocol_fault(struct smit_device *s, int error)
{
	if (!s->faulted)
		dev_warn(&s->udev->dev, "control unavailable generation=%u error=%d; %s\n",
			 s->generation, error,
			 s->recovery_attempts >= SMIT_RECOVERY_LIMIT
				 ? "automatic recovery exhausted; explicit recovery required"
				 : "rebuilding sessions");
	smit_protocol_invalidate(s);
	s->faulted = true;
	s->tune_error = error;
	s->recovery_at = jiffies + msecs_to_jiffies(1000);
}

int smit_protocol_send_apdu(struct smit_device *s, u16 session, const u8 *apdu, unsigned int len,
		     unsigned long deadline)
{
	u8 payload[255] = { 1, 0x90, 2 };
	if (!session || len > SMIT_MAX_APDU)
		return -EMSGSIZE;
	put_be16(payload + 3, session);
	memcpy(payload + 5, apdu, len);
	return smit_transport_request(s, 0xa0, payload, len + 5, deadline);
}

static struct smit_session *find_session(struct smit_device *s, u16 id)
{
	int i;
	for (i = 0; i < SMIT_MAX_SESSIONS; i++)
		if (s->sessions[i].id == id)
			return &s->sessions[i];
	return NULL;
}

static int open_session(struct smit_device *s, const u8 *p, unsigned long deadline)
{
	static const u8 profile_enquiry[] = { 0x9f, 0x80, 0x10, 0 };
	static const u8 app_enquiry[] = { 0x9f, 0x80, 0x20, 0 };
	static const u8 ca_enquiry[] = { 0x9f, 0x80, 0x30, 0 };
	static const u8 sas_connect[] = {
		0x9f, 0x9a, 0, 8, 'S', 'M', 'i', 'T', 'Z', 'B', 'J', 'L'
	};
	u8 reply[10] = { 1, 0x92, 7 };
	u32 resource = get_be32(p + 3);
	struct smit_session *session = NULL;
	int i, ret;

	memcpy(reply + 4, p + 3, 4);
	for (i = 0; i < SMIT_MAX_SESSIONS; i++)
		if (!s->sessions[i].id) {
			session = &s->sessions[i];
			break;
		}
	if (!session || s->next_session > 0xffff ||
	    (resource != SMIT_RESOURCE_MANAGER && resource != SMIT_RESOURCE_APP &&
	     resource != SMIT_RESOURCE_CA && resource != SMIT_RESOURCE_SAS)) {
		reply[3] = 0xf0; /* CI resource unavailable. */
		return smit_transport_request(s, 0xa0, reply, sizeof(reply), deadline);
	}
	session->id = s->next_session++;
	session->resource = resource;
	put_be16(reply + 8, session->id);
	ret = smit_transport_request(s, 0xa0, reply, sizeof(reply), deadline);
	if (ret)
		return ret;
	dev_dbg(&s->udev->dev, "session=%u resource=%08x generation=%u\n", session->id, resource,
		s->generation);
	switch (resource) {
	case SMIT_RESOURCE_MANAGER:
		return smit_protocol_send_apdu(s, session->id, profile_enquiry, sizeof(profile_enquiry),
				 deadline);
	case SMIT_RESOURCE_APP:
		return smit_protocol_send_apdu(s, session->id, app_enquiry, sizeof(app_enquiry), deadline);
	case SMIT_RESOURCE_CA:
		if (s->ca_session)
			return -EPROTO;
		s->ca_session = session->id;
		session->ready = true;
		return smit_protocol_send_apdu(s, session->id, ca_enquiry, sizeof(ca_enquiry), deadline);
	case SMIT_RESOURCE_SAS:
		/* The observed second SAS resource carries unsolicited identity data.
		 * Only the primary SAS resource performs the application handshake. */
		if (!s->sas_session) {
			s->sas_session = session->id;
			return smit_protocol_send_apdu(s, session->id, sas_connect, sizeof(sas_connect),
					 deadline);
		}
		session->ready = true;
		return 0;
	}
	return -EPROTO;
}

/* Returns 1 for a matching SAS reply, 0 for a dispatched event, or errno.
 * There is at most one synchronous smit_transport_request on this ordered worker. */
static int dispatch(struct smit_device *s, int len, u16 expected_session, u16 expected_type,
		    unsigned long deadline)
{
	static const u8 profile[] = {
		0x9f, 0x80, 0x11, 0x1c, 0, 1,	 0, 0x41, 0, 2,	   0,	 0x41, 0, 3,	0, 0x41,
		0,    0x24, 0,	  0x41, 0, 0x40, 0, 0x41, 0, 0x96, 0x10, 1,    0, 0x20, 0, 0x41
	}; /* Vendor resource
	      profile; do not infer
	      host MMI support from
	      it. */
	static const u8 profile_changed[] = { 0x9f, 0x80, 0x12, 0 };
	struct smit_session *session;
	u8 *p = s->data;
	u16 id, type;
	u32 tag;
	int body, offset;

	if (len < 3 || p[0] != 1)
		return -EPROTO;
	if (p[1] == 0x91)
		return len == 7 && p[2] == 4 ? open_session(s, p, deadline) : -EPROTO;
	if (p[1] == 0x95) {
		u8 reply[] = { 1, 0x96, 3, 0, 0, 0 };
		if (len != 5 || p[2] != 2)
			return -EPROTO;
		id = get_be16(p + 3);
		session = find_session(s, id);
		put_be16(reply + 4, id);
		if (!session)
			reply[3] = 0xf0;
		offset = smit_transport_request(s, 0xa0, reply, sizeof(reply), deadline);
		if (offset || !session)
			return offset;
		/* Rebuild the complete dependent resource graph, with fresh IDs. */
		return -ECONNRESET;
	}
	if (len < 9 || p[1] != 0x90 || p[2] != 2)
		return -EPROTO;
	id = get_be16(p + 3);
	session = find_session(s, id);
	if (!session) {
		dev_dbg(&s->udev->dev, "discard stale session=%u generation=%u\n", id,
			s->generation);
		return 0;
	}
	body = smit_apdu_body(p + 5, len - 5);
	if (body < 0)
		return body;
	body += 5;
	tag = (u32)p[5] << 16 | (u32)p[6] << 8 | p[7];
	if (session->resource == SMIT_RESOURCE_MANAGER) {
		if (tag == 0x9f8010)
			return smit_protocol_send_apdu(s, id, profile, sizeof(profile), deadline);
		if (tag == 0x9f8011)
			return smit_protocol_send_apdu(s, id, profile_changed, sizeof(profile_changed), deadline);
		return 0;
	}
	if (session->resource == SMIT_RESOURCE_CA) {
		if (id != s->ca_session)
			return 0;
		if (tag == 0x9f8031) {
			if ((len - body) % 2 || len - 5 > SMIT_MAX_APDU)
				return -EPROTO;
			memcpy(s->ca_info, p + 5, len - 5);
			s->ca_info_len = len - 5;
			s->ca_refresh = false;
			smit_ca_receive(s, p + 5, len - 5);
		} else if (tag == 0x9f8033) {
			/* Preserve raw reply; unknown codes are not entitlement success. */
			if (len - body < 4)
				return -EPROTO;
			smit_ca_receive(s, p + 5, len - 5);
		}
		return 0;
	}
	if (session->resource != SMIT_RESOURCE_SAS)
		return 0;
	if (tag == 0x9f9a01) {
		if (len - body != 9 || memcmp(p + body, "SMiTZBJL", 8) || p[body + 8])
			return -EPROTO;
		session->ready = true;
		return 0;
	}
	if (tag != 0x9f9a07)
		return 0;
	if (len - body < 7)
		return -EPROTO;
	type = get_be16(p + body + 3);
	offset = smit_sas_reply_session(p, len, id, type);
	if (offset < 0)
		return offset;
	if (type == SMIT_EVENT_CARD) {
		if (len == offset)
			return -EPROTO;
		s->card_event = p[offset];
		s->card_event_valid = true;
		s->ca_info_len = 0;
		s->ca_refresh = true;
		s->card_refresh = true;
		s->card_info[0] = 0;
		s->card_state = SMIT_CARD_UNKNOWN;
		smit_ca_invalidate(s);
		dev_info_ratelimited(
			&s->udev->dev,
			"card event raw=%02x generation=%u; CA information invalidated\n",
			s->card_event, s->generation);
		/* A card notification does not close the CI transport. Retiring live
		 * sessions here would force the cold handshake, which this firmware
		 * ignores until a firmware reboot. CA_INFO is capability, not entitlement. */
		return 0;
	}
	if (type == SMIT_EVENT_LOG) {
		dev_dbg(&s->udev->dev, "device log bytes=%d\n", len - offset);
		return 0;
	}
	if (id == expected_session && type == expected_type) {
		memmove(s->data, p + offset, len - offset);
		return len - offset + 1;
	}
	dev_dbg(&s->udev->dev, "unsolicited SAS session=%u type=%04x bytes=%d\n", id, type,
		len - offset);
	return 0;
}

int smit_protocol_pump(struct smit_device *s, u16 session, u16 type, unsigned long deadline)
{
	const u8 one = 1;
	int ret;
	if (!s->available) {
		ret = smit_transport_request(s, 0xa0, &one, 1, deadline);
		if (ret || !s->available)
			return ret;
	}
	ret = smit_transport_request(s, 0x81, &one, 1, deadline);
	if (ret < 0)
		return ret;
	return dispatch(s, ret, session, type, deadline);
}

int smit_protocol_initialize(struct smit_device *s)
{
	const u8 one = 1;
	unsigned long deadline = jiffies + msecs_to_jiffies(SMIT_INIT_TIMEOUT_MS);
	struct smit_session *primary;
	int ret, i;

	smit_protocol_invalidate(s);
	memset(s->sessions, 0, sizeof(s->sessions));
	s->sas_session = s->ca_session = 0;
	s->available = false;
	s->generation++;
	memcpy(s->tx, "\xfe\x00\x10\x00", 4);
	ret = smit_transport_exchange(s, 4, deadline);
	if (ret != 4 || memcmp(s->rx, "\xff\x00\x20\x00", 4))
		return ret < 0 ? ret : -EPROTO;
	ret = smit_transport_request(s, 0x82, &one, 1, deadline);
	if (ret)
		return ret;
	while (time_before(jiffies, deadline)) {
		ret = smit_protocol_pump(s, 0, 0, deadline);
		if (ret < 0)
			return ret;
		primary = find_session(s, s->sas_session);
		if (primary && primary->ready) {
			/* Drain bootstrap tail, including the second SAS open smit_transport_request.
			 * CA_INFO/card insertion is deliberately not a probe prerequisite. */
			for (i = 0; i < SMIT_DRAIN_BUDGET; i++) {
				ret = smit_protocol_pump(s, 0, 0, deadline);
				if (ret < 0)
					return ret;
				if (!s->available)
					break;
			}
			s->initialized = true;
			s->faulted = false;
			s->healthy_since = jiffies;
			s->tune_error = 0;
			s->card_refresh = true;
			dev_info(&s->udev->dev, "control ready generation=%u SAS=%u CA=%u\n",
				 s->generation, s->sas_session, s->ca_session);
			return 0;
		}
		msleep(10);
	}
	return -ETIMEDOUT;
}

int smit_protocol_sas_command(struct smit_device *s, u16 command, const u8 *data, unsigned int length,
		       u8 *reply, unsigned int capacity)
{
	u8 apdu[64] = { 0x9f, 0x9a, 7 };
	unsigned long deadline = jiffies + msecs_to_jiffies(SMIT_COMMAND_TIMEOUT_MS);
	u16 session = s->sas_session;
	int ret;
	if (length > sizeof(apdu) - 11)
		return -EMSGSIZE;
	apdu[3] = length + 7;
	apdu[4] = s->counter++;
	put_be16(apdu + 5, length + 4);
	put_be16(apdu + 7, command);
	put_be16(apdu + 9, length);
	memcpy(apdu + 11, data, length);
	ret = smit_protocol_send_apdu(s, session, apdu, length + 11, deadline);
	if (ret)
		goto failed;
	while (time_before(jiffies, deadline)) {
		ret = smit_protocol_pump(s, session, command + 1, deadline);
		if (ret < 0)
			goto failed;
		if (ret > 0) {
			ret--;
			if ((unsigned int)ret > capacity) {
				ret = -EMSGSIZE;
				goto failed;
			}
			memcpy(reply, s->data, ret);
			return ret;
		}
		msleep(10);
	}
	ret = -ETIMEDOUT;
failed:
	/* No echoed transaction ID: never issue another smit_transport_request on this session
	 * after an ambiguous outcome. Rebuild with never-reused session IDs. */
	smit_protocol_fault(s, ret);
	return ret;
}

int smit_protocol_result_command(struct smit_device *s, u16 command, const u8 *p, unsigned int len)
{
	u8 reply[4];
	int ret = smit_protocol_sas_command(s, command, p, len, reply, sizeof(reply));
	if (ret < 0)
		return ret;
	if (ret != sizeof(reply)) {
		smit_protocol_fault(s, -EPROTO);
		return -EPROTO;
	}
	if (get_be32(reply)) {
		dev_warn(&s->udev->dev, "SAS command=%04x device_result=%08x\n", command,
			 get_be32(reply));
		return -EREMOTEIO;
	}
	return 0;
}

int smit_protocol_tune(struct smit_device *s, u32 frequency, u32 symbol_rate)
{
	/* Vendor DVB-C/QAM64 tuple: kHz, kSym/s, QAM=64, bandwidth=8,
	 * cable selector=1 and two reserved zero bytes. Only this tuple is tested. */
	u8 data[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 8, 1, 0, 0 };
	int ret;
	s->tuned = false;
	put_le32(data, frequency / 1000);
	put_le32(data + 4, symbol_rate / 1000);
	ret = smit_protocol_result_command(s, SMIT_CMD_TUNE, data, sizeof(data));
	s->tune_error = ret;
	if (!ret) {
		s->frequency = frequency;
		s->symbol_rate = symbol_rate;
		s->tuned = true;
	}
	dev_info(&s->udev->dev, "smit_protocol_tune frequency=%u symbol_rate=%u result=%d (lock not implied)\n",
		 frequency, symbol_rate, ret);
	return ret;
}

int smit_protocol_status(struct smit_device *s, struct smit_signal_status *signal)
{
	u8 reply[16];
	int ret;
	memset(signal, 0, sizeof(*signal));
	if (!s->tuned)
		return s->tune_error;
	ret = smit_protocol_sas_command(s, SMIT_CMD_STATUS, (const u8 *)"GSTA", 4, reply, sizeof(reply));
	if (ret < 0)
		return ret;
	if (ret != sizeof(reply)) {
		smit_protocol_fault(s, -EPROTO);
		return -EPROTO;
	}
	if (get_le32(reply) != s->frequency / 1000 || get_le32(reply + 4) != s->symbol_rate / 1000)
		return -EAGAIN;
	signal->locked = reply[13] != 0;
	signal->level_dbuv = reply[14];
	signal->relative_strength = reply[15];
	signal->valid = true;
	return 0;
}

int smit_protocol_refresh_ca(struct smit_device *s)
{
	static const u8 enquiry[] = { 0x9f, 0x80, 0x30, 0 };
	int ret;
	if (!s->ca_session)
		return -EAGAIN;
	ret = smit_protocol_send_apdu(s, s->ca_session, enquiry, sizeof(enquiry),
			jiffies + msecs_to_jiffies(SMIT_COMMAND_TIMEOUT_MS));
	if (!ret)
		s->ca_refresh = false;
	return ret;
}

const char *smit_card_state_name(unsigned int state)
{
	switch (state) {
	case SMIT_CARD_ABSENT:
		return "absent";
	case SMIT_CARD_INVALID:
		return "invalid";
	case SMIT_CARD_WRONG:
		return "wrong_orientation";
	default:
		return "unknown";
	}
}

int smit_protocol_refresh_card(struct smit_device *s)
{
	u8 card[255];
	char normalized[256];
	bool changed;
	int ret, i;

	s->card_refresh = false;
	ret = smit_protocol_sas_command(s, SMIT_CMD_CARD_INFO, (const u8 *)"GCIN", 4, card, sizeof(card));
	s->card_check_at = jiffies + msecs_to_jiffies(SMIT_CARD_CHECK_MS);
	if (ret < 0)
		return ret;
	/* A notification interleaved with this query makes its text ambiguous.
	 * Keep the refresh flag set and query again before publishing card state. */
	if (s->card_refresh)
		return 0;
	changed = s->card_info_valid &&
		  (s->card_info_len != (unsigned int)ret || memcmp(s->card_info, card, ret));
	memcpy(s->card_info, card, ret);
	s->card_info[ret] = 0;
	s->card_info_len = ret;
	/* Firmware reports 'No Smart Card'; Android also knows underscore forms.
	 * Normalize ASCII case without changing the stored diagnostic bytes. */
	for (i = 0; i < ret; i++)
		normalized[i] = card[i] >= 'A' && card[i] <= 'Z' ? card[i] + ('a' - 'A') : card[i];
	normalized[ret] = 0;
	s->card_state = SMIT_CARD_UNKNOWN;
	if (strstr(normalized, "no_smartcard") || strstr(normalized, "no smart"))
		s->card_state = SMIT_CARD_ABSENT;
	else if (strstr(normalized, "invalid_smartcard") || strstr(normalized, "invalid smart"))
		s->card_state = SMIT_CARD_INVALID;
	else if (strstr(normalized, "installed_wrong") || strstr(normalized, "installed wrong"))
		s->card_state = SMIT_CARD_WRONG;
	if (!s->card_info_valid || changed)
		dev_info(&s->udev->dev,
			 "card information %s state=%s bytes=%d; entitlement unknown\n",
			 changed ? "changed" : "queried", smit_card_state_name(s->card_state), ret);
	s->card_info_valid = true;
	if (changed) {
		s->ca_info_len = 0;
		s->ca_refresh = true;
		smit_ca_invalidate(s);
	}
	return 0;
}

int smit_protocol_reboot_firmware(struct smit_device *s)
{
	int ret;

	/* Explicit recovery only. USB disconnect will retire this device instance;
	 * never resubmit automatically across re-enumeration and reset the budget. */
	smit_protocol_invalidate(s);
	s->reset_pending = true;
	s->reset_deadline = jiffies + msecs_to_jiffies(SMIT_REENUMERATION_MS);
	ret = usb_control_msg(s->udev, usb_sndctrlpipe(s->udev, 0), 0xa0,
			      USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE, 0, 0, NULL, 0,
			      SMIT_USB_TIMEOUT_MS);
	dev_info(&s->udev->dev,
		 "firmware reboot requested result=%d; waiting for USB re-enumeration\n", ret);
	/* Firmware can disconnect before the control transfer completes (-EPROTO
	 * was observed). Preserve the error; only a new device plus clear TS proves
	 * recovery, never this ioctl's return value alone. */
	return ret;
}
