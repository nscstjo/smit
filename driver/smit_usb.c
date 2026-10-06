#include "dvb_usb.h"
#include "smit_frontend.h"
#include "smit_device.h"
#include "../include/smit_identity.h"
DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);
static int smit_probe(struct dvb_usb_device *d)
{
	struct smit_device *s = d->priv;
	struct usb_host_interface *alt = d->intf->cur_altsetting;
	unsigned int found = 0;
	int i;
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;
		if (usb_endpoint_is_bulk_out(ep) && ep->bEndpointAddress == 0x01)
			found |= 1;
		if (usb_endpoint_is_bulk_in(ep) && ep->bEndpointAddress == 0x82)
			found |= 2;
		if (usb_endpoint_is_bulk_in(ep) && ep->bEndpointAddress == 0x84)
			found |= 4;
	}
	if (found != 7)
		return -ENODEV;
	s->udev = d->udev;
	return smit_start(s);
}
static int smit_frontend_attach(struct dvb_usb_adapter *adap)
{
	adap->fe[0] = smit_fe_attach(adap_to_priv(adap));
	return adap->fe[0] ? 0 : -ENOMEM;
}
static int smit_streaming_ctrl(struct dvb_frontend *fe, int onoff)
{
	struct smit_device *s = fe_to_priv(fe);
	int ret = 0;
	mutex_lock(&s->lock);
	if (onoff && !s->tuned)
		ret = -EAGAIN;
	/* Original driver has no device-side start/stop command. */
	dev_dbg(&s->udev->dev, "feed %s: %d\n", onoff ? "start" : "stop", ret);
	mutex_unlock(&s->lock);
	return ret;
}
static const struct dvb_usb_device_properties smit_props = {
	.driver_name = "smit",
	.owner = THIS_MODULE,
	.adapter_nr = adapter_nr,
	.bInterfaceNumber = 0,
	.size_of_priv = sizeof(struct smit_device),
	.probe = smit_probe,
	.frontend_attach = smit_frontend_attach,
	.init = smit_ca_register,
	.exit = smit_ca_unregister,
	.disconnect = smit_ca_unregister,
	.streaming_ctrl = smit_streaming_ctrl,
	.num_adapters = 1,
	.adapter = { { .stream = DVB_USB_STREAM_BULK(0x84, 8, 8192) } },
};
static const struct usb_device_id smit_ids[] = {
	{ DVB_USB_DEVICE(0x29df, 0x0001, &smit_props, SMIT_PRODUCT_NAME, NULL) }, {}
};
MODULE_DEVICE_TABLE(usb, smit_ids);

/* Diagnostic state only; never expose card identity or imply entitlement. */
static ssize_t smit_card_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dvb_usb_device *d = usb_get_intfdata(to_usb_interface(dev));
	struct smit_device *s;
	ssize_t n;

	if (!d)
		return -ENODEV;
	s = d->priv;
	mutex_lock(&s->lock);
	n = scnprintf(buf, PAGE_SIZE, "%s\n",
		      s->card_info_valid ? smit_card_state_name(s->card_state) : "unqueried");
	mutex_unlock(&s->lock);
	return n;
}
static DEVICE_ATTR_RO(smit_card_state);
static struct attribute *smit_attrs[] = { &dev_attr_smit_card_state.attr, NULL };
ATTRIBUTE_GROUPS(smit);

static int smit_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct dvb_usb_device *d = usb_get_intfdata(intf);
	int ret = dvb_usbv2_suspend(intf, message);
	if (ret)
		return ret;
	ret = smit_suspend(d->priv);
	if (ret)
		dvb_usbv2_resume(intf);
	return ret;
}

static int smit_usb_resume(struct usb_interface *intf)
{
	struct dvb_usb_device *d = usb_get_intfdata(intf);
	int ret = smit_resume(d->priv);
	int stream_ret = dvb_usbv2_resume(intf);

	/* Restore framework streaming even when control recovery is pending.
	 * The bounded protocol worker can then recover without stranded URBs. */
	return ret ? ret : stream_ret;
}

static int smit_usb_reset_resume(struct usb_interface *intf)
{
	struct dvb_usb_device *d = usb_get_intfdata(intf);
	struct smit_device *s = d->priv;

	mutex_lock(&s->lock);
	s->initialized = false; /* USB reset invalidates the old SAS session. */
	mutex_unlock(&s->lock);
	/* Do not call dvb_usbv2_reset_resume: its props->init would register ca0
	 * again. Our resume rebuilds protocol state while retaining the DVB nodes. */
	return smit_usb_resume(intf);
}

static struct usb_driver smit_driver = {
	.name = "smit",
	.id_table = smit_ids,
	.probe = dvb_usbv2_probe,
	.disconnect = dvb_usbv2_disconnect,
	.suspend = smit_usb_suspend,
	.resume = smit_usb_resume,
	.reset_resume = smit_usb_reset_resume,
	.dev_groups = smit_groups,
};
module_usb_driver(smit_driver);
MODULE_DESCRIPTION(SMIT_PRODUCT_NAME);
MODULE_LICENSE("GPL");
