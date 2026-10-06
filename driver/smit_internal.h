#ifndef SMIT_INTERNAL_H
#define SMIT_INTERNAL_H
#include "smit_device.h"
#ifndef SMIT_PROTOCOL_TEST
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/completion.h>
#endif

/* Private module interfaces. All control I/O runs on the ordered worker
 * with device->lock held; these symbols are never exported to other modules. */
#define SMIT_USB_TIMEOUT_MS 500
#define SMIT_COMMAND_TIMEOUT_MS 5000
#define SMIT_INIT_TIMEOUT_MS 15000
#define SMIT_POLL_MS 100
#define SMIT_CARD_CHECK_MS 5000	    /* Fallback for firmware without card notifications. */
#define SMIT_REENUMERATION_MS 10000 /* Bound waiting after a firmware reboot request. */
#define SMIT_DRAIN_BUDGET 16	    /* Fairness: a noisy device cannot starve callers. */
#define SMIT_RECOVERY_LIMIT 3
#define SMIT_RECOVERY_STABLE_MS 30000 /* Brief success must not defeat retry limits. */
#define SMIT_RESOURCE_MANAGER 0x00010041
#define SMIT_RESOURCE_APP 0x00020041
#define SMIT_RESOURCE_CA 0x00030041
#define SMIT_RESOURCE_SAS 0x00961001

/* SAS request IDs; the corresponding reply ID is request + 1. */
enum smit_sas_command {
	SMIT_CMD_RESET = 0x0001,
	SMIT_CMD_TUNE = 0x0003,
	SMIT_CMD_STATUS = 0x0005,
	SMIT_CMD_STANDBY = 0x0009,
	SMIT_CMD_RESUME = 0x000b,
	SMIT_CMD_CARD_INFO = 0x0010,
	SMIT_CMD_TYPE = 0x0021,
	SMIT_CMD_HARDWARE = 0x1007,
};

#define SMIT_EVENT_LOG 0x1005
#define SMIT_EVENT_CARD 0x100c


extern bool trace, probe_diagnostics, experimental_power;
extern bool experimental_ep0_reset, experimental_long_tx;

static inline u16 get_be16(const u8 *p)
{
	return (u16)p[0] << 8 | p[1];
}
static inline u32 get_be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static inline u32 get_le32(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static inline void put_be16(u8 *p, u16 n)
{
	p[0] = n >> 8;
	p[1] = n;
}
static inline void put_le32(u8 *p, u32 n)
{
	p[0] = n;
	p[1] = n >> 8;
	p[2] = n >> 16;
	p[3] = n >> 24;
}


int smit_transport_exchange(struct smit_device *s, int txlen, unsigned long deadline);
int smit_transport_request(struct smit_device *s, u8 req, const u8 *data, unsigned int len,
		   unsigned long deadline);
void smit_protocol_invalidate(struct smit_device *s);
void smit_protocol_fault(struct smit_device *s, int error);
int smit_protocol_send_apdu(struct smit_device *s, u16 session, const u8 *apdu, unsigned int len,
		     unsigned long deadline);
int smit_protocol_pump(struct smit_device *s, u16 session, u16 type, unsigned long deadline);
int smit_protocol_initialize(struct smit_device *s);
int smit_protocol_sas_command(struct smit_device *s, u16 command, const u8 *data, unsigned int length,
		       u8 *reply, unsigned int capacity);
int smit_protocol_result_command(struct smit_device *s, u16 command, const u8 *p, unsigned int len);
int smit_protocol_tune(struct smit_device *s, u32 frequency, u32 symbol_rate);
int smit_protocol_status(struct smit_device *s, struct smit_signal_status *signal);
int smit_protocol_refresh_ca(struct smit_device *s);
int smit_protocol_refresh_card(struct smit_device *s);
int smit_protocol_reboot_firmware(struct smit_device *s);
#endif
