#define _GNU_SOURCE
#include "server.h"
static void *worker(void *arg) {
    int fd=(int)(intptr_t)arg; handle(fd); close(fd);
    pthread_mutex_lock(&lock); clients--; pthread_cond_broadcast(&drained); pthread_mutex_unlock(&lock); return NULL;
}
int main(int argc,char **argv) {
    const char *bind_ip="127.0.0.1"; unsigned long port=38212; int i,server,opt=1; struct sockaddr_in addr={.sin_family=AF_INET};
    for(i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--mock")) mock=1;
        else if(!strcmp(argv[i],"--bind")&&i+1<argc) bind_ip=argv[++i];
        else if(!strcmp(argv[i],"--adapter")&&i+1<argc) adapter=argv[++i];
        else if(!strcmp(argv[i],"--port")&&i+1<argc) { char *end; errno=0; port=strtoul(argv[++i],&end,10); if(errno||*end||!port||port>65535) return 2; }
        else { fprintf(stderr,SMIT_PRODUCT_NAME "\nusage: %s [--bind IPv4] [--port 38212] [--adapter /dev/dvb/adapter0] [--mock]\n",argv[0]); return !strcmp(argv[i],"--help")?0:2; }
    }
    if(inet_pton(AF_INET,bind_ip,&addr.sin_addr)!=1) return 2;
    addr.sin_port=htons((uint16_t)port);
    { struct sigaction a={0}; a.sa_handler=terminate; sigemptyset(&a.sa_mask); sigaction(SIGINT,&a,NULL); sigaction(SIGTERM,&a,NULL); signal(SIGPIPE,SIG_IGN); }
    server=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0); if(server<0) { perror("socket"); return 1; }
    setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    if(bind(server,(struct sockaddr *)&addr,sizeof(addr))||listen(server,16)) { perror("listen"); close(server); return 1; }
    fprintf(stderr,SMIT_PRODUCT_NAME " listening on %s:%lu (%s)\n",bind_ip,port,mock?"MOCK":"Linux DVB");
    while(!quitting) {
        struct pollfd p={server,POLLIN,0}; pthread_t t; int fd;
        if(poll(&p,1,100)<=0) continue;
        fd=accept4(server,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC); if(fd<0) continue;
        pthread_mutex_lock(&lock);
        if(clients>=MAX_CLIENTS) { pthread_mutex_unlock(&lock); error_response(fd,503,EBUSY,"client limit"); close(fd); continue; }
        clients++; pthread_mutex_unlock(&lock);
        if(pthread_create(&t,NULL,worker,(void *)(intptr_t)fd)) { close(fd); pthread_mutex_lock(&lock); clients--; pthread_mutex_unlock(&lock); }
        else pthread_detach(t);
    }
    close(server); pthread_mutex_lock(&lock); while(clients) pthread_cond_wait(&drained,&lock);
    if(fe>=0) close(fe);
    if(ca>=0) close(ca);
    pthread_mutex_unlock(&lock); return 0;
}
