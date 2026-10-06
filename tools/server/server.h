#ifndef SMIT_SERVER_H
#define SMIT_SERVER_H
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dvb/ca.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../../include/smit_identity.h"
#define LIMIT 16384
#define MAX_CLIENTS 16
#include "json.h"
extern volatile sig_atomic_t quitting;
extern pthread_mutex_t lock, stream_lock;
extern pthread_cond_t drained, stream_drained;
extern const char *adapter;
extern int mock, fe, ca, clients, streaming, generation, stopping_stream;
extern unsigned frequency, symbol_rate, selected;
extern uint64_t stream_bytes, stream_errors, stream_sessions;
extern int stream_errno;

double clock_s(void);
void terminate(int sig);
int node(const char *name, int mode);
int device(int *fd, const char *name);
int send_all(int fd, const void *buf, size_t n);
void response(int fd,int status,const char *body);
void error_response(int fd,int status,int e,const char *msg);
int dispatch(const char *path,struct object *o,char *out,size_t cap);
void stream(int fd);
void handle(int fd);
extern const char smit_api[];
#endif
