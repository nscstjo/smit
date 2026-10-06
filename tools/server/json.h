#ifndef SMIT_JSON_H
#define SMIT_JSON_H
#include <stdint.h>
struct field { char key[40], text[2049]; uint32_t number; int string; };
struct object { struct field f[8]; unsigned n; };
int parse(const char *p,struct object *o);
struct field *get(struct object *o,const char *key);
int number(struct object *o,const char *key,unsigned min,unsigned max,unsigned def,unsigned *value);
int keys(struct object *o,const char *allowed);
#endif
