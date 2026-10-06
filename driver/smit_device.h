#ifndef SMIT_DEVICE_H
#define SMIT_DEVICE_H
#ifdef SMIT_PROTOCOL_TEST
#include "../tests/kernel_runtime.h"
#else
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#endif

/* USB endpoint addresses, not CI resource or session numbers. */
#define SMIT_BULK_ENDPOINT_CMD_SEND 0x01
#define SMIT_BULK_ENDPOINT_CMD_RECEIVE 0x82
#define SMIT_BULK_ENDPOINT_STREAM 0x84
#define SMIT_MAX_SESSIONS 8
#define SMIT_MAX_APDU 250 /* 255-byte transport payload minus session header. */
struct smit_session {
	u32 resource;
	u16 id;
	bool ready;
};
struct smit_device {
	struct usb_device *udev;
	/* Worker owns command USB I/O. Readers of state also take lock. */
	struct mutex lock;
	struct workqueue_struct *wq;
	struct delayed_work poll_work;
	bool stopping, suspended, initialized, tuned, faulted, ca_refresh, card_refresh;
	bool card_info_valid, reset_pending;
	int tune_error;
	unsigned int recovery_attempts;
	unsigned long recovery_at, healthy_since;
	unsigned long card_check_at, reset_deadline;
	u32 generation, next_session;
	struct smit_session sessions[SMIT_MAX_SESSIONS];
	u16 sas_session, ca_session;
	u8 counter;
	struct smit_ca *ca;
	u8 ca_info[SMIT_MAX_APDU];
	unsigned int ca_info_len;
	u8 card_event;
	bool card_event_valid;
	/* GCIN text is diagnostic data, never proof of entitlement. */
	char card_info[256];
	unsigned int card_info_len;
	enum { SMIT_CARD_UNKNOWN, SMIT_CARD_ABSENT, SMIT_CARD_INVALID, SMIT_CARD_WRONG } card_state;
	u8 tx[260], rx[1000], data[256];
	bool available;
	u32 frequency, symbol_rate, wanted_frequency, wanted_symbol_rate;
};
int smit_start(struct smit_device *s);
void smit_stop(struct smit_device *s);
int smit_tune(struct smit_device *s, u32 frequency, u32 symbol_rate);
struct smit_signal_status {
	bool valid, locked;
	u8 level_dbuv;
	u8 relative_strength; /* Firmware calls this snr; it is not CNR. */
};
int smit_status(struct smit_device *s, struct smit_signal_status *signal);
int smit_ca_message(struct smit_device *s, const u8 *apdu, unsigned int len);
int smit_reset(struct smit_device *s);
int smit_suspend(struct smit_device *s);
int smit_resume(struct smit_device *s);
const char *smit_card_state_name(unsigned int state);
struct dvb_usb_device;
int smit_ca_register(struct dvb_usb_device *d);
void smit_ca_unregister(struct dvb_usb_device *d);
/* Worker callbacks never take the ioctl/lifetime mutex. */
void smit_ca_receive(struct smit_device *s, const u8 *apdu, unsigned int len);
void smit_ca_invalidate(struct smit_device *s);
#endif
