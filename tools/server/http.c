#define _GNU_SOURCE
#include "server.h"
void handle(int fd) {
    char buf[LIMIT+1],path[128],method[16],version[16],out[8192]; size_t used=0,header=0,body=0; int has_length=0,content_type=0;
    double end=clock_s()+5; struct object obj;
    while(!quitting) {
        ssize_t n; struct pollfd p={fd,POLLIN,0}; char *e;
        if(clock_s()>end) { error_response(fd,408,ETIMEDOUT,"request timeout"); return; }
        if(used==LIMIT) { error_response(fd,413,EMSGSIZE,"request too large"); return; }
        if(poll(&p,1,100)<=0) continue;
        n=recv(fd,buf+used,LIMIT-used,0);
        if(n<0&&(errno==EINTR||errno==EAGAIN)) continue;
        if(n<=0) { if(used) error_response(fd,400,EINVAL,"truncated request"); return; }
        if(memchr(buf+used,0,(size_t)n)) { error_response(fd,400,EINVAL,"NUL in request"); return; }
        used+=(size_t)n; buf[used]=0;
        if(!header && (e=strstr(buf,"\r\n\r\n"))) {
            char *line,*next,*request_end=strstr(buf,"\r\n"); char extra;
            header=(size_t)(e-buf)+4;
            if(header>8192) { error_response(fd,431,EMSGSIZE,"headers too large"); return; }
            *request_end=0;
            if(sscanf(buf,"%15s %127s %15s %c",method,path,version,&extra)!=3 || (strcmp(version,"HTTP/1.1")&&strcmp(version,"HTTP/1.0"))) { error_response(fd,400,EINVAL,"invalid request line"); return; }
            line=request_end+2;
            while(line<e) {
                char *colon,*v,*tail; unsigned long length;
                next=strstr(line,"\r\n"); if(!next) { error_response(fd,400,EINVAL,"invalid header"); return; } *next=0;
                colon=strchr(line,':'); if(!colon) { error_response(fd,400,EINVAL,"invalid header"); return; } *colon=0; v=colon+1; while(*v==' '||*v=='\t') v++;
                if(!strcasecmp(line,"Content-Length")) {
                    if(has_length++||!isdigit((unsigned char)*v)) { error_response(fd,400,EINVAL,"invalid content length"); return; }
                    errno=0; length=strtoul(v,&tail,10); while(*tail==' '||*tail=='\t') tail++;
                    if(errno||*tail||length>LIMIT-header) { error_response(fd,413,EMSGSIZE,"invalid body length"); return; } body=(size_t)length;
                } else if(!strcasecmp(line,"Transfer-Encoding")) { error_response(fd,400,EINVAL,"transfer encoding unsupported"); return; }
                else if(!strcasecmp(line,"Content-Type")) { content_type=!strcasecmp(v,"application/json")||!strncasecmp(v,"application/json;",17); }
                else if(!strcasecmp(line,"Origin")) { error_response(fd,403,EACCES,"browser origins are not allowed"); return; }
                line=next+2;
            }
        }
        if(header && used>=header+body) break;
    }
    if(quitting) return;
    if(used!=header+body) { error_response(fd,400,EINVAL,"unexpected trailing bytes"); return; }
    if(!strcmp(method,"GET")) {
        if(body) { error_response(fd,400,EINVAL,"GET body unsupported"); return; }
        if(!strcmp(path,"/health")) { snprintf(out,sizeof(out),"{\"ok\":true,\"mock\":%s,\"version\":1,\"product\":\"" SMIT_PRODUCT_NAME "\"}",mock?"true":"false"); response(fd,200,out); }
        else if(!strcmp(path,"/api")) response(fd,200,smit_api);
        else if(!strcmp(path,"/stream")) stream(fd);
        else error_response(fd,(!strcmp(path,"/frontend/info") || !strcmp(path,"/frontend/status") || !strcmp(path,"/frontend/tune") || !strcmp(path,"/ca/caps") || !strcmp(path,"/ca/slot") || !strcmp(path,"/ca/send") || !strcmp(path,"/ca/receive") || !strcmp(path,"/ca/reset") || !strcmp(path,"/demux/section") || !strcmp(path,"/service/select") || !strcmp(path,"/stream/stop") || !strcmp(path,"/stream/stats")) ? 405 : 404,ENOENT,"unknown path or wrong method");
    } else if(!strcmp(method,"POST")) {
        int status,e;
        if(!has_length) { error_response(fd,411,EINVAL,"Content-Length required"); return; }
        if(!content_type) { error_response(fd,415,EINVAL,"application/json required"); return; }
        if(parse(buf+header,&obj)) { error_response(fd,400,EINVAL,"invalid JSON object"); return; }
        pthread_mutex_lock(&lock); errno=0; status=dispatch(path,&obj,out,sizeof(out)); e=errno; pthread_mutex_unlock(&lock);
        if(status==200) response(fd,status,out); else error_response(fd,status,e,status==400?"invalid parameters":status==404?"unknown path":status==409?"stop the active stream first":"DVB operation failed");
    } else error_response(fd,405,EINVAL,"method not allowed");
}
