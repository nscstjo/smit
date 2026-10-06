#define _GNU_SOURCE
#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
static void space(const char **p) { while(**p==' '||**p=='\t'||**p=='\r'||**p=='\n') ++*p; }
static int string_value(const char **p,char *out,size_t cap) {
    size_t n=0; if(*(*p)++!='"') return -1;
    while(**p && **p!='"') {
        unsigned char c=(unsigned char)*(*p)++;
        if(c<32 || c>126 || c=='\\' || n+1>=cap) return -1;
        out[n++]=(char)c;
    }
    if(**p!='"') return -1;
    ++*p; out[n]=0; return 0;
}
int parse(const char *p,struct object *o) {
    memset(o,0,sizeof(*o)); space(&p); if(*p++!='{') return -1; space(&p);
    if(*p=='}') { ++p; space(&p); return *p?-1:0; }
    for(;;) {
        struct field *f; unsigned i; uint64_t v=0;
        if(o->n==8) return -1;
        f=&o->f[o->n]; if(string_value(&p,f->key,sizeof(f->key)) || !f->key[0]) return -1;
        for(i=0;i<o->n;i++) if(!strcmp(o->f[i].key,f->key)) return -1;
        space(&p); if(*p++!=':') return -1; space(&p);
        if(*p=='"') { f->string=1; if(string_value(&p,f->text,sizeof(f->text))) return -1; }
        else {
            if(!isdigit((unsigned char)*p) || (*p=='0' && isdigit((unsigned char)p[1]))) return -1;
            do { v=v*10+(unsigned)(*p++-'0'); if(v>UINT32_MAX) return -1; } while(isdigit((unsigned char)*p));
            f->number=(uint32_t)v;
        }
        o->n++; space(&p); if(*p=='}') { ++p; space(&p); return *p?-1:0; }
        if(*p++!=',') return -1;
        space(&p);
    }
}
struct field *get(struct object *o,const char *key) { unsigned i; for(i=0;i<o->n;i++) if(!strcmp(o->f[i].key,key)) return &o->f[i]; return NULL; }
int number(struct object *o,const char *key,unsigned min,unsigned max,unsigned def,unsigned *value) {
    struct field *f=get(o,key); if(!f) { *value=def; return def<min||def>max?-1:0; }
    if(f->string||f->number<min||f->number>max) return -1;
    *value=f->number; return 0;
}
int keys(struct object *o,const char *allowed) {
    unsigned i; for(i=0;i<o->n;i++) { char k[44]; snprintf(k,sizeof(k),"|%s|",o->f[i].key); if(!strstr(allowed,k)) return -1; } return 0;
}
