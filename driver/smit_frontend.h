#ifndef SMIT_FRONTEND_H
#define SMIT_FRONTEND_H
#include <media/dvb_frontend.h>
struct smit_device;
struct dvb_frontend *smit_fe_attach(struct smit_device *s);
#endif
