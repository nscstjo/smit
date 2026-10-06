/* High-level CI APDU interface. No host control-word handling. */
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/dvb/ca.h>
#include "dvb_usb.h"
#include "smit_device.h"
#include "smit_ca_queue.h"

struct smit_ca {
	struct kref refs;
	/* Serializes ioctl with detach; RX never takes this mutex. */
	struct mutex lock;
	spinlock_t rx_lock;
	wait_queue_head_t wait;
	struct smit_device *s;
	struct dvb_device *dev;
	struct smit_ca_queue rx;
	bool dead;
};

static void ca_free(struct kref *ref)
{
	kfree(container_of(ref, struct smit_ca, refs));
}

/* Called with s->lock held. Detach stops/flushed the worker before freeing ca. */
void smit_ca_receive(struct smit_device *s, const u8 *apdu, unsigned int len)
{
	struct smit_ca *ca = s->ca;
	unsigned long flags;
	if (!ca || len > SMIT_MAX_APDU)
		return;
	spin_lock_irqsave(&ca->rx_lock, flags);
	smit_ca_queue_push(&ca->rx, apdu, len);
	spin_unlock_irqrestore(&ca->rx_lock, flags);
	wake_up_interruptible(&ca->wait);
}

void smit_ca_invalidate(struct smit_device *s)
{
	struct smit_ca *ca = s->ca;
	unsigned long flags;
	if (!ca)
		return;
	spin_lock_irqsave(&ca->rx_lock, flags);
	smit_ca_queue_invalidate(&ca->rx);
	spin_unlock_irqrestore(&ca->rx_lock, flags);
	wake_up_interruptible(&ca->wait);
}

static int ca_open(struct inode *inode, struct file *file)
{
	struct dvb_device *dev = file->private_data;
	struct smit_ca *ca = dev->priv;
	int ret = dvb_generic_open(inode, file);
	if (!ret)
		kref_get(&ca->refs);
	return ret;
}
static int ca_release(struct inode *inode, struct file *file)
{
	struct dvb_device *dev = file->private_data;
	struct smit_ca *ca = dev->priv;
	int ret = dvb_generic_release(inode, file);
	kref_put(&ca->refs, ca_free);
	return ret;
}
static __poll_t ca_poll(struct file *file, poll_table *wait)
{
	struct smit_ca *ca = ((struct dvb_device *)file->private_data)->priv;
	unsigned long flags;
	__poll_t mask = 0;
	poll_wait(file, &ca->wait, wait);
	spin_lock_irqsave(&ca->rx_lock, flags);
	if (ca->dead)
		mask |= EPOLLHUP | EPOLLERR;
	if (ca->rx.invalidated || ca->rx.overflow)
		mask |= EPOLLERR;
	if (ca->rx.count)
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&ca->rx_lock, flags);
	return mask;
}

static int ca_ioctl(struct file *file, unsigned int cmd, void *arg)
{
	struct smit_ca *ca = ((struct dvb_device *)file->private_data)->priv;
	struct ca_msg *msg = arg;
	unsigned long flags;
	int ret = 0;
	mutex_lock(&ca->lock);
	if (!ca->s) {
		ret = -ENODEV;
		goto out;
	}
	switch (cmd) {
	case CA_GET_CAP:
		memset(arg, 0, sizeof(struct ca_caps));
		((struct ca_caps *)arg)->slot_num = 1;
		((struct ca_caps *)arg)->slot_type = CA_CI;
		break;
	case CA_GET_SLOT_INFO: {
		struct ca_slot_info *slot = arg;
		if (slot->num) {
			ret = -EINVAL;
			break;
		}
		mutex_lock(&ca->s->lock);
		slot->type = CA_CI;
		/* Embedded CI module presence is distinct from smart-card presence. */
		slot->flags = READ_ONCE(ca->s->stopping) ? 0 : CA_CI_MODULE_PRESENT;
		if (ca->s->initialized && !ca->s->faulted && !ca->s->suspended && ca->s->ca_session)
			slot->flags |= CA_CI_MODULE_READY;
		mutex_unlock(&ca->s->lock);
		break;
	}
	case CA_RESET:
		ret = file->f_mode & FMODE_WRITE ? smit_reset(ca->s) : -EBADF;
		break;
	case CA_SEND_MSG:
		if (!(file->f_mode & FMODE_WRITE)) {
			ret = -EBADF;
			break;
		}
		if (msg->index || msg->type || msg->length > sizeof(msg->msg)) {
			ret = -EINVAL;
			break;
		}
		ret = smit_ca_message(ca->s, msg->msg, msg->length);
		break;
	case CA_GET_MSG:
		if (!(file->f_mode & FMODE_READ)) {
			ret = -EBADF;
			break;
		}
		spin_lock_irqsave(&ca->rx_lock, flags);
		ret = smit_ca_queue_pop(&ca->rx, msg);
		spin_unlock_irqrestore(&ca->rx_lock, flags);
		break;
	default:
		ret = -EOPNOTSUPP;
	}
out:
	mutex_unlock(&ca->lock);
	return ret;
}
static const struct file_operations ca_fops = {
	.owner = THIS_MODULE,
	.open = ca_open,
	.release = ca_release,
	.poll = ca_poll,
	.unlocked_ioctl = dvb_generic_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = dvb_generic_ioctl,
#endif
};
static const struct dvb_device ca_template = {
	.users = 1,
	.readers = 1,
	.writers = 1,
	.fops = &ca_fops,
	.kernel_ioctl = ca_ioctl,
};

int smit_ca_register(struct dvb_usb_device *d)
{
	struct smit_device *s = d->priv;
	struct smit_ca *ca;
	int ret;
	ca = kzalloc(sizeof(*ca), GFP_KERNEL);
	if (!ca)
		return -ENOMEM;
	kref_init(&ca->refs);
	mutex_init(&ca->lock);
	spin_lock_init(&ca->rx_lock);
	init_waitqueue_head(&ca->wait);
	ca->s = s;
	/* Publish the queue before dvb_register_device exposes the node. */
	mutex_lock(&s->lock);
	s->ca = ca;
	if (s->ca_info_len)
		smit_ca_receive(s, s->ca_info, s->ca_info_len);
	mutex_unlock(&s->lock);
	ret = dvb_register_device(&d->adapter[0].dvb_adap, &ca->dev, &ca_template, ca,
				  DVB_DEVICE_CA, 0);
	if (ret) {
		mutex_lock(&s->lock);
		s->ca = NULL;
		mutex_unlock(&s->lock);
		kref_put(&ca->refs, ca_free);
		return ret;
	}
	return 0;
}

void smit_ca_unregister(struct dvb_usb_device *d)
{
	struct smit_device *s = d->priv;
	struct smit_ca *ca;
	unsigned long flags;
	if (!s)
		return;
	smit_stop(s);
	ca = s->ca;
	if (!ca)
		return;
	mutex_lock(&ca->lock);
	ca->s = NULL;
	s->ca = NULL;
	spin_lock_irqsave(&ca->rx_lock, flags);
	ca->dead = true;
	ca->rx.count = 0;
	spin_unlock_irqrestore(&ca->rx_lock, flags);
	mutex_unlock(&ca->lock);
	wake_up_interruptible(&ca->wait);
	dvb_unregister_device(ca->dev);
	kref_put(&ca->refs, ca_free);
}
