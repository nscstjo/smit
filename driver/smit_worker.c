#include "smit_internal.h"
#include "smit_ca_pmt.h"

bool trace;
module_param(trace, bool, 0600);
MODULE_PARM_DESC(trace, "Dump control frames (may contain card identity); disabled by default");
bool probe_diagnostics;
module_param(probe_diagnostics, bool, 0400);
MODULE_PARM_DESC(probe_diagnostics,
		 "Issue optional GCIN/GDHW/GTYP probe queries; raw replies require trace=1");
bool experimental_power;
module_param(experimental_power, bool, 0400);
MODULE_PARM_DESC(experimental_power,
		 "Enable unvalidated REST via CA_RESET and STBY/RESU during system sleep");
bool experimental_ep0_reset;
module_param(experimental_ep0_reset, bool, 0400);
MODULE_PARM_DESC(
	experimental_ep0_reset,
	"CA_RESET requests a firmware reboot via EP0; DVB nodes will disconnect and reconnect");
bool experimental_long_tx;
module_param(experimental_long_tx, bool, 0400);
MODULE_PARM_DESC(experimental_long_tx,
		 "Enable unvalidated BER long USB OUT payloads (128..255 bytes)");

static void poll_worker(struct work_struct *work)
{
	struct smit_device *s = container_of(to_delayed_work(work), struct smit_device, poll_work);
	unsigned long deadline;
	int i, ret = 0;

	mutex_lock(&s->lock);
	if (READ_ONCE(s->stopping) || s->suspended)
		goto out;
	if (s->reset_pending) {
		if (time_after_eq(jiffies, s->reset_deadline)) {
			s->reset_pending = false;
			s->recovery_attempts = SMIT_RECOVERY_LIMIT;
			smit_protocol_fault(s, -ETIMEDOUT);
			dev_warn(&s->udev->dev,
				 "firmware did not re-enumerate; explicit recovery required\n");
		}
		goto again;
	}
	if (s->faulted) {
		if (s->recovery_attempts >= SMIT_RECOVERY_LIMIT ||
		    time_before(jiffies, s->recovery_at))
			goto again;
		s->recovery_attempts++;
		ret = smit_protocol_initialize(s);
		if (!ret && s->wanted_frequency)
			ret = smit_protocol_tune(s, s->wanted_frequency, s->wanted_symbol_rate);
		if (ret) {
			smit_protocol_fault(s, ret);
			s->recovery_at = jiffies + msecs_to_jiffies(1000U << s->recovery_attempts);
			dev_warn(&s->udev->dev, "recovery attempt=%u/%u error=%d%s\n",
				 s->recovery_attempts, SMIT_RECOVERY_LIMIT, ret,
				 s->recovery_attempts == SMIT_RECOVERY_LIMIT
					 ? "; automatic recovery exhausted"
					 : "");
		}
		goto again;
	}
	deadline = jiffies + msecs_to_jiffies(SMIT_COMMAND_TIMEOUT_MS);
	for (i = 0; i < SMIT_DRAIN_BUDGET; i++) {
		ret = smit_protocol_pump(s, 0, 0, deadline);
		if (ret < 0 || !s->available)
			break;
	}
	if (ret >= 0 && s->ca_refresh)
		ret = smit_protocol_refresh_ca(s);
	if (ret >= 0 && (s->card_refresh || time_after_eq(jiffies, s->card_check_at)))
		ret = smit_protocol_refresh_card(s);
	if (ret < 0)
		smit_protocol_fault(s, ret);
	else if (time_after_eq(jiffies,
			       s->healthy_since + msecs_to_jiffies(SMIT_RECOVERY_STABLE_MS)))
		s->recovery_attempts = 0;
again:
	if (!READ_ONCE(s->stopping))
		queue_delayed_work(s->wq, &s->poll_work, msecs_to_jiffies(SMIT_POLL_MS));
out:
	mutex_unlock(&s->lock);
}

enum operation { OP_INIT, OP_TUNE, OP_STATUS, OP_CA, OP_RESET, OP_SUSPEND, OP_RESUME };
struct command_job {
	struct work_struct work;
	struct completion done;
	struct smit_device *s;
	enum operation op;
	u32 frequency, symbol_rate;
	const u8 *apdu;
	unsigned int len;
	struct smit_signal_status signal;
	int result;
};

static void command_worker(struct work_struct *work)
{
	struct command_job *job = container_of(work, struct command_job, work);
	struct smit_device *s = job->s;
	u8 diagnostic[256];
	int ret = 0;

	mutex_lock(&s->lock);
	if (READ_ONCE(s->stopping)) {
		ret = -ENODEV;
		goto out;
	}
	if (job->op == OP_INIT) {
		ret = smit_protocol_initialize(s);
		if (!ret && probe_diagnostics) {
			ret = smit_protocol_sas_command(s, SMIT_CMD_CARD_INFO, (const u8 *)"GCIN", 4, diagnostic,
					  sizeof(diagnostic));
			if (ret >= 0)
				ret = smit_protocol_sas_command(s, SMIT_CMD_HARDWARE, (const u8 *)"GDHW", 4,
						  diagnostic, sizeof(diagnostic));
			if (ret >= 0)
				ret = smit_protocol_sas_command(s, SMIT_CMD_TYPE, (const u8 *)"GTYP", 4,
						  diagnostic, sizeof(diagnostic));
			if (ret >= 0)
				ret = 0;
		}
		goto out;
	}
	if (s->reset_pending && job->op != OP_SUSPEND) {
		if (job->op == OP_RESUME) {
			s->suspended = false;
			queue_delayed_work(s->wq, &s->poll_work, msecs_to_jiffies(SMIT_POLL_MS));
		}
		ret = -EAGAIN;
		goto out;
	}
	if (job->op == OP_RESET && experimental_ep0_reset) {
		ret = smit_protocol_reboot_firmware(s);
		goto out;
	}
	if (job->op == OP_RESUME) {
		s->suspended = false;
		s->recovery_attempts = 0;
		if (experimental_power && s->initialized)
			ret = smit_protocol_result_command(s, SMIT_CMD_RESUME, (const u8 *)"RESU", 4);
		/* REST/RESU operate on the tuner, not the CI transport. The device
		 * ignores the cold bootstrap handshake on a live transport. Retain
		 * confirmed sessions; reset_resume marks initialized=false instead. */
		if (!ret && !s->initialized)
			ret = smit_protocol_initialize(s);
		if (!ret) {
			s->ca_refresh = true;
			s->card_refresh = true;
		}
		if (!ret && s->wanted_frequency)
			ret = smit_protocol_tune(s, s->wanted_frequency, s->wanted_symbol_rate);
		if (ret)
			smit_protocol_fault(s, ret);
		queue_delayed_work(s->wq, &s->poll_work, msecs_to_jiffies(SMIT_POLL_MS));
		goto out;
	}
	/* A failed/uninitialized peripheral must not prevent system sleep. */
	if (job->op == OP_SUSPEND) {
		if (experimental_power && s->initialized && !s->faulted)
			ret = smit_protocol_result_command(s, SMIT_CMD_STANDBY, (const u8 *)"STBY", 4);
		if (!ret) {
			s->suspended = true;
			s->tuned = false;
			s->ca_info_len = 0;
			s->card_info[0] = 0;
			s->card_state = SMIT_CARD_UNKNOWN;
			s->card_event_valid = false;
			smit_ca_invalidate(s);
		}
		goto out;
	}
	if (s->suspended) {
		ret = -EHOSTDOWN;
		goto out;
	}
	if (job->op == OP_RESET) {
		if (!experimental_power) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		if (!s->initialized || s->faulted) {
			s->recovery_attempts = 0;
			ret = smit_protocol_initialize(s);
			if (ret) {
				smit_protocol_fault(s, ret);
				goto out;
			}
		}
	}
	if (!s->initialized || s->faulted) {
		ret = -EAGAIN;
		goto out;
	}
	switch (job->op) {
	case OP_TUNE:
		s->wanted_frequency = job->frequency;
		s->wanted_symbol_rate = job->symbol_rate;
		ret = smit_protocol_tune(s, job->frequency, job->symbol_rate);
		break;
	case OP_STATUS:
		ret = smit_protocol_status(s, &job->signal);
		break;
	case OP_CA:
		if (!s->ca_session) {
			ret = -EAGAIN;
			break;
		}
		if (job->len == 4 && !memcmp(job->apdu, "\x9f\x80\x30\0", 4)) {
			if (s->ca_info_len)
				smit_ca_receive(s, s->ca_info, s->ca_info_len);
			ret = smit_protocol_refresh_ca(s);
		} else if (!s->tuned) {
			ret = -EAGAIN;
		} else {
			ret = smit_protocol_send_apdu(s, s->ca_session, job->apdu, job->len,
					jiffies + msecs_to_jiffies(SMIT_COMMAND_TIMEOUT_MS));
		}
		if (ret && ret != -EAGAIN && ret != -EMSGSIZE)
			smit_protocol_fault(s, ret);
		break;
	case OP_RESET:
		ret = smit_protocol_result_command(s, SMIT_CMD_RESET, (const u8 *)"REST", 4);
		if (!ret) {
			/* A successful tuner reset does not close its SAS session. */
			s->tuned = false;
			s->ca_info_len = 0;
			s->ca_refresh = true;
			s->card_refresh = true;
			smit_ca_invalidate(s);
		}
		if (!ret && s->wanted_frequency)
			ret = smit_protocol_tune(s, s->wanted_frequency, s->wanted_symbol_rate);
		if (ret)
			smit_protocol_fault(s, ret);
		break;
	default:
		ret = -EINVAL;
	}
out:
	job->result = ret;
	mutex_unlock(&s->lock);
	complete(&job->done);
}

static int call(struct smit_device *s, struct command_job *job)
{
	int ret;
	job->s = s;
	init_completion(&job->done);
	INIT_WORK(&job->work, command_worker);
	ret = mutex_lock_interruptible(&s->lock);
	if (ret)
		return -EINTR;
	if (READ_ONCE(s->stopping) || !s->wq) {
		mutex_unlock(&s->lock);
		return -ENODEV;
	}
	queue_work(s->wq, &job->work);
	mutex_unlock(&s->lock);
	/* Stack job cannot outlive caller; USB and protocol waits are bounded.
	 * Disconnect sets stopping before flushing, so every queued job completes. */
	if (wait_for_completion_interruptible(&job->done)) {
		cancel_work_sync(&job->work);
		return -EINTR; /* Never transparently restart a state-changing command. */
	}
	flush_work(&job->work); /* Worker must relinquish the caller's stack job. */
	return job->result;
}

int smit_start(struct smit_device *s)
{
	struct command_job job = { .op = OP_INIT };
	int ret;
	mutex_init(&s->lock);
	s->next_session = 1;
	INIT_DELAYED_WORK(&s->poll_work, poll_worker);
	s->wq = alloc_ordered_workqueue("smit-control", WQ_MEM_RECLAIM);
	if (!s->wq)
		return -ENOMEM;
	ret = call(s, &job);
	if (ret) {
		smit_stop(s);
		return ret;
	}
	queue_delayed_work(s->wq, &s->poll_work, msecs_to_jiffies(SMIT_POLL_MS));
	return 0;
}

void smit_stop(struct smit_device *s)
{
	struct workqueue_struct *wq;
	if (!READ_ONCE(s->wq))
		return;
	/* Interrupt bounded pumping before waiting for the state lock. */
	WRITE_ONCE(s->stopping, true);
	mutex_lock(&s->lock);
	wq = s->wq;
	mutex_unlock(&s->lock);
	if (!wq)
		return;
	cancel_delayed_work_sync(&s->poll_work);
	flush_workqueue(wq);
	mutex_lock(&s->lock);
	s->wq = NULL;
	smit_protocol_invalidate(s);
	mutex_unlock(&s->lock);
	destroy_workqueue(wq);
}

int smit_tune(struct smit_device *s, u32 frequency, u32 symbol_rate)
{
	struct command_job job = { .op = OP_TUNE,
				   .frequency = frequency,
				   .symbol_rate = symbol_rate };
	if (frequency < 10000000 || frequency > 862000000 || frequency % 1000 ||
	    symbol_rate < 1000 || symbol_rate > 10000000 || symbol_rate % 1000)
		return -EINVAL;
	return call(s, &job);
}
int smit_status(struct smit_device *s, struct smit_signal_status *signal)
{
	struct command_job job = { .op = OP_STATUS };
	int ret = call(s, &job);
	memset(signal, 0, sizeof(*signal));
	if (!ret)
		*signal = job.signal;
	return ret;
}
int smit_ca_message(struct smit_device *s, const u8 *apdu, unsigned int len)
{
	struct command_job job = { .op = OP_CA, .apdu = apdu, .len = len };
	if (len > SMIT_MAX_APDU)
		return -EMSGSIZE;
	if (!(len == 4 && !memcmp(apdu, "\x9f\x80\x30\0", 4)) && !smit_ca_pmt_valid(apdu, len))
		return -EINVAL;
	return call(s, &job);
}
int smit_reset(struct smit_device *s)
{
	struct command_job job = { .op = OP_RESET };
	return call(s, &job);
}
int smit_suspend(struct smit_device *s)
{
	struct command_job job = { .op = OP_SUSPEND };
	int ret = call(s, &job);
	if (!ret)
		cancel_delayed_work_sync(&s->poll_work);
	return ret;
}
int smit_resume(struct smit_device *s)
{
	struct command_job job = { .op = OP_RESUME };
	return call(s, &job);
}
