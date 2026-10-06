#include <linux/slab.h>
#include "smit_frontend.h"
#include "smit_device.h"
#include "../include/smit_identity.h"
struct smit_state {
	struct dvb_frontend fe;
	struct smit_device *s;
	bool relative_valid;
	u16 relative_strength;
};
static void smit_clear_stats(struct dvb_frontend *fe)
{
	struct smit_state *st = fe->demodulator_priv;
	struct dtv_frontend_properties *p = &fe->dtv_property_cache;

	p->strength.len = 1;
	p->strength.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	p->strength.stat[0].svalue = 0;
	p->cnr.len = 1;
	p->cnr.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	p->cnr.stat[0].uvalue = 0;
	st->relative_valid = false;
	st->relative_strength = 0;
}
static void smit_release(struct dvb_frontend *fe)
{
	kfree(fe->demodulator_priv);
}
static int smit_set_frontend(struct dvb_frontend *fe)
{
	struct smit_state *st = fe->demodulator_priv;
	struct dtv_frontend_properties *p = &fe->dtv_property_cache;
	if (p->delivery_system != SYS_DVBC_ANNEX_A || p->modulation != QAM_64)
		return -EINVAL;
	smit_clear_stats(fe);
	return smit_tune(st->s, p->frequency, p->symbol_rate);
}
static int smit_get_frontend(struct dvb_frontend *fe, struct dtv_frontend_properties *p)
{
	struct smit_state *st = fe->demodulator_priv;
	mutex_lock(&st->s->lock);
	p->frequency = st->s->frequency;
	p->symbol_rate = st->s->symbol_rate;
	p->delivery_system = SYS_DVBC_ANNEX_A;
	p->modulation = QAM_64;
	mutex_unlock(&st->s->lock);
	return 0;
}
static int smit_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
	struct smit_state *st = fe->demodulator_priv;
	struct dtv_frontend_properties *p = &fe->dtv_property_cache;
	struct smit_signal_status signal;
	int ret;
	*status = 0;
	smit_clear_stats(fe);
	ret = smit_status(st->s, &signal);
	if (ret || !signal.valid)
		return ret;
	/* DVB strength uses milli-dBm. For the 75-ohm cable input:
	 * dBm = dBuV - 90 - 10*log10(75) = dBuV - 108.751.
	 */
	p->strength.stat[0].scale = FE_SCALE_DECIBEL;
	p->strength.stat[0].svalue = (s32)signal.level_dbuv * 1000 - 108751;
	if (signal.relative_strength <= 100) {
		st->relative_strength = (u32)signal.relative_strength * 65535 / 100;
		st->relative_valid = true;
	}
	/* These are strength measurements even when the demodulator is unlocked.
	 * The firmware's snr byte supplies no carrier/noise ratio.
	 */
	if (signal.locked)
		*status = FE_HAS_SIGNAL | FE_HAS_CARRIER | FE_HAS_VITERBI | FE_HAS_SYNC | FE_HAS_LOCK;
	return 0;
}
static int smit_read_signal_strength(struct dvb_frontend *fe, u16 *strength)
{
	struct smit_state *st = fe->demodulator_priv;
	enum fe_status status;
	int ret;

	*strength = 0;
	ret = smit_read_status(fe, &status);
	if (ret)
		return ret;
	if (!st->relative_valid)
		return -EAGAIN;
	*strength = st->relative_strength;
	return 0;
}
static int smit_tune_settings(struct dvb_frontend *fe, struct dvb_frontend_tune_settings *settings)
{
	settings->min_delay_ms = 800;
	settings->step_size = 0;
	settings->max_drift = 0;
	return 0;
}
static const struct dvb_frontend_ops smit_ops = {
	.delsys = { SYS_DVBC_ANNEX_A },
	.info = {
		.name = SMIT_PRODUCT_NAME,
		.frequency_min_hz = 10000000, .frequency_max_hz = 862000000,
		.frequency_stepsize_hz = 10000,
		.symbol_rate_min = 1000, .symbol_rate_max = 10000000,
		.caps = FE_CAN_QAM_64 | FE_CAN_FEC_AUTO,
	},
	.release = smit_release, .set_frontend = smit_set_frontend,
	.get_frontend = smit_get_frontend, .get_tune_settings = smit_tune_settings,
	.read_status = smit_read_status,
	.read_signal_strength = smit_read_signal_strength,
};
struct dvb_frontend *smit_fe_attach(struct smit_device *s)
{
	struct smit_state *st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return NULL;
	st->s = s;
	st->fe.ops = smit_ops;
	st->fe.demodulator_priv = st;
	smit_clear_stats(&st->fe);
#ifdef CONFIG_MEDIA_ATTACH
	/* Embedded frontend: its symbol cannot be looked up while this module
	 * is COMING. Balance core's dvb_detach(release) explicitly instead.
	 * USB unbind must release this reference before module removal.
	 */
	__module_get(THIS_MODULE);
#endif
	return &st->fe;
}
