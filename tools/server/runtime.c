#define _GNU_SOURCE
#include "server.h"
volatile sig_atomic_t quitting;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t drained = PTHREAD_COND_INITIALIZER;
/* Control ioctls can block; the DVR reader uses a separate state lock. */
pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t stream_drained = PTHREAD_COND_INITIALIZER;
const char *adapter = "/dev/dvb/adapter0";
int mock, fe = -1, ca = -1, clients, streaming, generation, stopping_stream;
unsigned frequency, symbol_rate, selected;
uint64_t stream_bytes, stream_errors, stream_sessions;
int stream_errno;
double clock_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
void terminate(int sig) { (void)sig; quitting=1; }
int node(const char *name, int mode) {
    char path[1024];
    if (snprintf(path,sizeof(path),"%s/%s",adapter,name)>=(int)sizeof(path)) { errno=ENAMETOOLONG; return -1; }
    return open(path,mode|O_NONBLOCK|O_CLOEXEC);
}
int device(int *fd, const char *name) { if (*fd<0) *fd=node(name,O_RDWR); return *fd; }
int send_all(int fd, const void *buf, size_t n) {
    const char *p=buf; double deadline=clock_s()+3;
    while(n && !quitting) {
        ssize_t r=send(fd,p,n,MSG_NOSIGNAL);
        if(r>0) { p+=r; n-=r; continue; }
        if(r<0 && errno==EINTR) continue;
        if(r<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
            struct pollfd f={fd,POLLOUT,0};
            if(clock_s()>=deadline) { errno=ETIMEDOUT; return -1; }
            if(poll(&f,1,100)>=0) continue;
        }
        return -1;
    }
    if(n) { errno=EINTR; return -1; } return 0;
}
void response(int fd,int status,const char *body) {
    char h[512];
    int n=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n",status,status==200?"OK":"Error",strlen(body));
    if(send_all(fd,h,(size_t)n)==0) (void)send_all(fd,body,strlen(body));
}
void error_response(int fd,int status,int e,const char *msg) {
    char b[512]; snprintf(b,sizeof(b),"{\"ok\":false,\"errno\":%d,\"error\":\"%s\"}",e,msg); response(fd,status,b);
}
