#define _GNU_SOURCE
#include "server.h"
void stream(int fd) {
    int dvr=-1,dmx=-1,gen,err=0; uint8_t data[188*256]; unsigned cc=0;
    struct dmx_pes_filter_params f={.pid=0x2000,.input=DMX_IN_FRONTEND,.output=DMX_OUT_TS_TAP,.pes_type=DMX_PES_OTHER,.flags=DMX_IMMEDIATE_START};
    pthread_mutex_lock(&lock);
    pthread_mutex_lock(&stream_lock);
    if(streaming || stopping_stream) { pthread_mutex_unlock(&stream_lock); pthread_mutex_unlock(&lock); error_response(fd,409,EBUSY,"one raw stream is already active"); return; }
    pthread_mutex_unlock(&stream_lock);
    if(!mock) {
        dvr=node("dvr0",O_RDONLY); if(dvr<0) err=errno;
        if(!err) { dmx=node("demux0",O_RDWR); if(dmx<0 || ioctl(dmx,DMX_SET_PES_FILTER,&f)<0) err=errno; }
        if(err) { if(dvr>=0) close(dvr); if(dmx>=0) close(dmx); pthread_mutex_unlock(&lock); error_response(fd,502,err,"DVB stream open failed"); return; }
    }
    pthread_mutex_lock(&stream_lock);
    streaming=1; gen=generation; stream_sessions++; stream_bytes=0; stream_errno=0;
    pthread_mutex_unlock(&stream_lock);
    pthread_mutex_unlock(&lock);
    { const char *h="HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"; if(send_all(fd,h,strlen(h))) err=errno; }
    while(!quitting && !err) {
        ssize_t n; int cancel; struct pollfd p[2]={{dvr,POLLIN,0},{fd,POLLIN,0}};
        pthread_mutex_lock(&stream_lock); cancel=gen!=generation; pthread_mutex_unlock(&stream_lock); if(cancel) break;
        if(poll(p,2,mock?10:100)<0) { if(errno==EINTR) continue; err=errno; break; }
        if(p[1].revents) break; /* EOF, error or unexpected pipelined bytes. */
        if(mock) { unsigned i; for(i=0;i<sizeof(data);i+=188) { memset(data+i,255,188); data[i]=0x47; data[i+1]=0x1f; data[i+2]=0xff; data[i+3]=0x10|(cc++&15); } n=sizeof(data); }
        else {
            if(p[0].revents&(POLLERR|POLLHUP|POLLNVAL)) { err=EIO; break; }
            if(!(p[0].revents&POLLIN)) continue;
            n=read(dvr,data,sizeof(data));
            if(n<0&&(errno==EAGAIN||errno==EINTR)) continue;
            if(n<=0) { err=n<0?errno:EIO; break; }
        }
        if(send_all(fd,data,(size_t)n)) { err=errno; break; }
        pthread_mutex_lock(&stream_lock); stream_bytes+=(uint64_t)n; pthread_mutex_unlock(&stream_lock);
    }
    if(dmx>=0) { (void)ioctl(dmx,DMX_STOP); close(dmx); } if(dvr>=0) close(dvr);
    pthread_mutex_lock(&stream_lock); streaming=0; pthread_cond_broadcast(&stream_drained); if(err) { stream_errors++; stream_errno=err; } pthread_mutex_unlock(&stream_lock);
}
