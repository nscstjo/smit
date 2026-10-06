#define _GNU_SOURCE
#include "server.h"
#include "program_specific_information.h"
static void hex(char *out,const uint8_t *p,unsigned n) { unsigned i; for(i=0;i<n;i++) sprintf(out+2*i,"%02x",p[i]); out[n*2]=0; }
static int unhex(const char *p,uint8_t *out,unsigned max) {
    unsigned i,n=(unsigned)strlen(p); if(n%2||!n||n>max*2) return -1;
    for(i=0;i<n;i+=2) { unsigned v; if(!isxdigit((unsigned char)p[i])||!isxdigit((unsigned char)p[i+1])) return -1; sscanf(p+i,"%2x",&v); out[i/2]=(uint8_t)v; } return (int)n/2;
}
static int section(unsigned pid,unsigned table,uint8_t *buf,double deadline) {
    int fd=node("demux0",O_RDWR), ret=-1, saved;
    struct dmx_sct_filter_params f={0};
    if(fd<0) return -1;
    f.pid=pid; f.filter.filter[0]=table; f.filter.mask[0]=255; f.flags=DMX_IMMEDIATE_START|DMX_CHECK_CRC;
    if(ioctl(fd,DMX_SET_FILTER,&f)<0) goto done;
    while(!quitting && clock_s()<deadline) {
        struct pollfd p={fd,POLLIN,0}; int r=poll(&p,1,100);
        if(r<0 && errno==EINTR) continue;
        if(r<0) goto done;
        if(p.revents&(POLLERR|POLLHUP|POLLNVAL)) { errno=EIO; goto done; }
        if(p.revents&POLLIN) {
            ret=(int)read(fd,buf,1024);
            if(ret<0 && (errno==EAGAIN||errno==EINTR)) continue;
            if(ret==0) { ret=-1; errno=EIO; } goto done;
        }
    }
    errno=quitting?EINTR:ETIMEDOUT;
done: saved=errno; close(fd); errno=saved; return ret;
}
static int select_service(unsigned id,unsigned timeout,char *out,size_t cap) {
    uint8_t pat[1024],pmt[1024]; int pn, mn, pid=-1; struct service_psi s; double end=clock_s()+timeout/1000.0;
    do { pn=section(0,0,pat,end); if(pn<0) return -1; pid=psi_find_pmt(pat,(unsigned)pn,id); } while(pid<0 && clock_s()<end);
    if(pid<0) { errno=ENOENT; return -1; }
    mn=section((unsigned)pid,2,pmt,end); if(mn<0) return -1;
    if(psi_build(&s,pat,(unsigned)pn,pmt,(unsigned)mn,id)) { errno=EPROTO; return -1; }
    if(s.has_ca) {
        struct ca_msg msg={0}; struct ca_caps caps={0};
        if(device(&ca,"ca0")<0 || ioctl(ca,CA_GET_CAP,&caps)<0) return -1;
        if(!(caps.slot_type&CA_CI)) { errno=EOPNOTSUPP; return -1; }
        msg.length=s.capmt_len; memcpy(msg.msg,s.capmt,s.capmt_len);
        if(ioctl(ca,CA_SEND_MSG,&msg)<0) return -1;
    }
    selected=id;
    { unsigned i; size_t n=(size_t)snprintf(out,cap,"{\"ok\":true,\"service\":%u,\"pmt_pid\":%u,\"pcr_pid\":%u,\"ca_submitted\":%s,\"clear_verified\":false,\"streams\":[",id,s.pmt_pid,s.pcr,s.has_ca?"true":"false");
      for(i=0;i<s.count;i++) n+=(size_t)snprintf(out+n,cap-n,"%s{\"pid\":%u,\"type\":%u}",i?",":"",s.pid[i],s.type[i]);
      snprintf(out+n,cap-n,"]}"); }
    return 0;
}
const char smit_api[]="{\"ok\":true,\"version\":1,\"get\":[\"/health\",\"/api\",\"/stream\"],\"post\":[\"/frontend/info\",\"/frontend/status\",\"/frontend/tune\",\"/ca/caps\",\"/ca/slot\",\"/ca/send\",\"/ca/receive\",\"/ca/reset\",\"/demux/section\",\"/service/select\",\"/stream/stop\",\"/stream/stats\"]}";
/* Called under lock; one hardware transaction at a time. */
int dispatch(const char *path,struct object *o,char *out,size_t cap) {
    unsigned a,b,c; int rc=0;
    strcpy(out,"{\"ok\":true}");
#define ROUTE(p) (!strcmp(path,p))
#define CHECK(x) do { if(x) { errno=EINVAL; return 400; } } while(0)
#define IO(x) do { if((x)<0) return 502; } while(0)
#define BUSY() do { int active; pthread_mutex_lock(&stream_lock); active=streaming; pthread_mutex_unlock(&stream_lock); if(active) { errno=EBUSY; return 409; } } while(0)
    if(ROUTE("/frontend/tune")) {
        CHECK(keys(o,"|frequency||symbol_rate||modulation|"));
        CHECK(number(o,"frequency",1000000,1000000000,0,&a)||number(o,"symbol_rate",1000,10000000,0,&b)||number(o,"modulation",16,256,64,&c));
        CHECK(c!=16&&c!=32&&c!=64&&c!=128&&c!=256); BUSY();
        if(!mock) {
            struct dtv_property p[7]={{.cmd=DTV_CLEAR},{.cmd=DTV_DELIVERY_SYSTEM,.u.data=SYS_DVBC_ANNEX_A},{.cmd=DTV_FREQUENCY,.u.data=a},{.cmd=DTV_SYMBOL_RATE,.u.data=b},{.cmd=DTV_MODULATION,.u.data=c==16?QAM_16:c==32?QAM_32:c==64?QAM_64:c==128?QAM_128:QAM_256},{.cmd=DTV_INVERSION,.u.data=INVERSION_AUTO},{.cmd=DTV_TUNE}};
            struct dtv_properties props={7,p}; IO(device(&fe,"frontend0")); IO(ioctl(fe,FE_SET_PROPERTY,&props));
        }
        frequency=a; symbol_rate=b; selected=0;
        snprintf(out,cap,"{\"ok\":true,\"frequency\":%u,\"symbol_rate\":%u,\"submitted\":true}",a,b);
    } else if(ROUTE("/frontend/info")) {
        struct dvb_frontend_info info={0}; CHECK(keys(o,""));
        if(!mock) { IO(device(&fe,"frontend0")); IO(ioctl(fe,FE_GET_INFO,&info)); }
        /* name from kernel is encoded, never interpolated into JSON. */
        { char name[257]; hex(name,(uint8_t *)info.name,(unsigned)strnlen(info.name,sizeof(info.name))); snprintf(out,cap,"{\"ok\":true,\"name_hex\":\"%s\",\"type\":%u,\"frequency_min\":%u,\"frequency_max\":%u,\"caps\":%u}",name,info.type,info.frequency_min,info.frequency_max,info.caps); }
    } else if(ROUTE("/frontend/status")) {
        fe_status_t status=FE_HAS_LOCK; CHECK(keys(o,""));
        if(!mock) { IO(device(&fe,"frontend0")); IO(ioctl(fe,FE_READ_STATUS,&status)); }
        snprintf(out,cap,"{\"ok\":true,\"status\":%u,\"locked\":%s,\"frequency\":%u,\"symbol_rate\":%u,\"service\":%u",status,status&FE_HAS_LOCK?"true":"false",frequency,symbol_rate,selected);
        { size_t used=strlen(out); struct dtv_property p[2]={{.cmd=DTV_STAT_SIGNAL_STRENGTH},{.cmd=DTV_STAT_CNR}}; struct dtv_properties props={2,p};
          int available=!mock && ioctl(fe,FE_GET_PROPERTY,&props)==0;
          unsigned i; for(i=0;i<2;i++) {
              struct dtv_stats *s=&p[i].u.st.stat[0];
              int valid=available && p[i].result==0 && p[i].u.st.len>0 && s->scale!=FE_SCALE_NOT_AVAILABLE;
              if(valid) used+=(size_t)snprintf(out+used,cap-used,",\"%s\":{\"scale\":%u,\"value\":%" PRId64 "}",i?"cnr":"signal",s->scale,(int64_t)(s->scale==FE_SCALE_DECIBEL?s->svalue:(int64_t)s->uvalue));
              else used+=(size_t)snprintf(out+used,cap-used,",\"%s\":null",i?"cnr":"signal");
          } snprintf(out+used,cap-used,"}"); }
    } else if(ROUTE("/ca/caps")) {
        struct ca_caps v={.slot_num=1,.slot_type=CA_CI}; CHECK(keys(o,""));
        if(!mock) { IO(device(&ca,"ca0")); IO(ioctl(ca,CA_GET_CAP,&v)); }
        snprintf(out,cap,"{\"ok\":true,\"slot_num\":%u,\"slot_type\":%u,\"descr_num\":%u,\"descr_type\":%u}",v.slot_num,v.slot_type,v.descr_num,v.descr_type);
    } else if(ROUTE("/ca/slot")) {
        struct ca_slot_info v={0}; CHECK(keys(o,"|slot|")); CHECK(number(o,"slot",0,255,0,&a)); v.num=(int)a;
        if(!mock) { IO(device(&ca,"ca0")); IO(ioctl(ca,CA_GET_SLOT_INFO,&v)); }
        snprintf(out,cap,"{\"ok\":true,\"slot\":%d,\"type\":%d,\"flags\":%u}",v.num,v.type,v.flags);
    } else if(ROUTE("/ca/send")) {
        struct ca_msg msg={0}; struct field *f=get(o,"hex"); int n;
        CHECK(keys(o,"|hex|")); CHECK(!f||!f->string); n=unhex(f->text,msg.msg,250); CHECK(n<4); msg.length=(unsigned)n;
        if(!mock) { IO(device(&ca,"ca0")); IO(ioctl(ca,CA_SEND_MSG,&msg)); }
        snprintf(out,cap,"{\"ok\":true,\"bytes\":%u}",msg.length);
    } else if(ROUTE("/ca/receive")) {
        struct ca_msg msg={0}; char h[513]; CHECK(keys(o,""));
        if(!mock) { IO(device(&ca,"ca0")); IO(ioctl(ca,CA_GET_MSG,&msg)); }
        else { msg.length=6; memcpy(msg.msg,"\x9f\x80\x31\x02\x12\x34",6); }
        if(msg.length>256) { errno=EPROTO; return 502; }
        hex(h,msg.msg,msg.length); snprintf(out,cap,"{\"ok\":true,\"hex\":\"%s\",\"tag\":%u,\"bytes\":%u}",h,msg.length>=3?((unsigned)msg.msg[0]<<16 | (unsigned)msg.msg[1]<<8 | msg.msg[2]):0,msg.length);
    } else if(ROUTE("/ca/reset")) {
        CHECK(keys(o,"|confirm|")); CHECK(number(o,"confirm",1,1,0,&a)); BUSY();
        if(!mock) { IO(device(&ca,"ca0")); IO(ioctl(ca,CA_RESET,0)); } selected=0;
    } else if(ROUTE("/demux/section")) {
        uint8_t data[1024]; char h[2049]; int n;
        CHECK(keys(o,"|pid||table||timeout_ms|")); CHECK(number(o,"pid",0,8191,UINT32_MAX,&a)||number(o,"table",0,255,UINT32_MAX,&b)||number(o,"timeout_ms",100,5000,1000,&c));
        if(mock) { n=16; memset(data,0,16); data[0]=(uint8_t)b; data[5]=0xc1; psi_finish(data,12); }
        else { n=section(a,b,data,clock_s()+c/1000.0); IO(n); }
        hex(h,data,(unsigned)n); snprintf(out,cap,"{\"ok\":true,\"pid\":%u,\"table\":%u,\"bytes\":%d,\"hex\":\"%s\"}",a,b,n,h);
    } else if(ROUTE("/service/select")) {
        CHECK(keys(o,"|service||timeout_ms|")); CHECK(number(o,"service",1,65535,0,&a)||number(o,"timeout_ms",100,10000,3000,&b)); BUSY();
        if(mock) { selected=a; snprintf(out,cap,"{\"ok\":true,\"service\":%u,\"pmt_pid\":100,\"pcr_pid\":101,\"ca_submitted\":true,\"clear_verified\":false,\"streams\":[{\"pid\":101,\"type\":27}]}",a); }
        else { rc=select_service(a,b,out,cap); IO(rc); }
    } else if(ROUTE("/stream/stats")) {
        CHECK(keys(o,""));
        pthread_mutex_lock(&stream_lock);
        snprintf(out,cap,"{\"ok\":true,\"active\":%s,\"bytes\":%" PRIu64 ",\"errors\":%" PRIu64 ",\"last_errno\":%d,\"sessions\":%" PRIu64 "}",streaming?"true":"false",stream_bytes,stream_errors,stream_errno,stream_sessions);
        pthread_mutex_unlock(&stream_lock);
    } else if(ROUTE("/stream/stop")) {
        CHECK(keys(o,""));
        pthread_mutex_lock(&stream_lock);
        stopping_stream=1; generation++;
        while(streaming) pthread_cond_wait(&stream_drained,&stream_lock);
        stopping_stream=0;
        pthread_mutex_unlock(&stream_lock);
    }
    else { errno=ENOENT; return ROUTE("/health")||ROUTE("/api")||ROUTE("/stream")?405:404; }
    return 200;
}
