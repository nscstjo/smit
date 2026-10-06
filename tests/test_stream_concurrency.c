/* Offline regression: a blocked control ioctl must not block raw DVR drain. */
#define main smit_server_program_main
#include "../tools/server/runtime.c"
#include "../tools/server/json.c"
#include "../tools/server/dvb_control.c"
#include "../tools/server/transport_stream.c"
#include "../tools/server/http.c"
#include "../tools/server/main.c"

#undef main
#include <assert.h>

static void *run_stream(void *arg) { stream((int)(intptr_t)arg); return NULL; }

int main(void) {
    int sockets[2], rc; pthread_t thread; uint8_t buf[188*256];
    uint64_t before, after; struct pollfd p;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)==0);
    mock=1;
    assert(pthread_create(&thread,NULL,run_stream,(void *)(intptr_t)sockets[0])==0);
    p=(struct pollfd){sockets[1],POLLIN,0};
    assert(poll(&p,1,2000)==1);
    assert(read(sockets[1],buf,sizeof(buf))>0);
    pthread_mutex_lock(&lock); /* simulate section/FE ioctl holding control lock */
    pthread_mutex_lock(&stream_lock); before=stream_bytes; pthread_mutex_unlock(&stream_lock);
    double end=clock_s()+.25;
    while(clock_s()<end) {
        p.revents=0;
        rc=poll(&p,1,20);
        if(rc>0) assert(read(sockets[1],buf,sizeof(buf))>0);
    }
    pthread_mutex_lock(&stream_lock); after=stream_bytes; pthread_mutex_unlock(&stream_lock);
    pthread_mutex_unlock(&lock);
    assert(after>before+sizeof(buf)*3);
    pthread_mutex_lock(&stream_lock); generation++; pthread_mutex_unlock(&stream_lock);
    close(sockets[1]);
    assert(pthread_join(thread,NULL)==0);
    close(sockets[0]);
    assert(!streaming);
    puts("control lock leaves raw stream progressing: passed");
    return 0;
}
