#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../tools/server/program_specific_information.h"
int main(void)
{
    uint8_t pat[16]={0,0xb0,13,0,42,0xc1,0,0,0,115,0xe0,115};
    uint8_t pmt[32]={2,0xb0,29,0,115,0xc1,0,0,0xe0,32,0xf0,6,
        9,4,0x4a,2,0xe4,0x70,0x1b,0xe0,32,0xf0,0,4,0xe0,33,0xf0,0};
    struct service_psi s;
    unsigned n;
    psi_finish(pat,12); psi_finish(pmt,28);
    assert(!psi_build(&s,pat,sizeof(pat),pmt,sizeof(pmt),115));
    assert(s.has_ca && s.count==2 && s.pid[0]==32 && s.pid[1]==33 && s.pcr==32);
    assert(s.capmt_len==27 && s.pmt_len==26);
    assert(!memcmp(s.capmt,"\x9f\x80\x32\x17\x03\x00\x73\xc1\xf0\x07\x01\x09\x04\x4a\x02\xe4\x70",17));
    assert(psi_valid(s.pat,16,0) && psi_valid(s.pmt,s.pmt_len,2));
    assert(!s.pmt[11]);
    {
        uint8_t es_ca[32]={2,0xb0,29,0,115,0xc1,0,0,0xe0,32,0xf0,0,
            0x1b,0xe0,32,0xf0,6,9,4,0x4a,2,0xe4,0x70,4,0xe0,33,0xf0,0};
        psi_finish(es_ca,28);
        assert(!psi_build(&s,pat,16,es_ca,32,115));
        assert(s.has_ca && s.capmt_len==27 && s.pmt_len==26 && !s.pmt[16]);
        es_ca[17]=0x80; psi_finish(es_ca,28); /* private descriptor, FTA path */
        assert(!psi_build(&s,pat,16,es_ca,32,115));
        assert(!s.has_ca && !s.capmt_len && s.pmt_len==32 && s.pmt[16]==6);
    }
    {
        uint8_t large[149]={0};
        memcpy(large,pmt,12); large[11]=128;
        memcpy(large+12,pmt+12,6); large[18]=0x80; large[19]=120;
        memcpy(large+140,pmt+18,5); psi_finish(large,145);
        assert(psi_build(&s,pat,16,large,sizeof(large),115)==-2);
    }
    assert(psi_build(&s,pat,16,pmt,32,116)<0);
    for (n=0;n<32;n++) {
        uint8_t *short_pmt=malloc(n?n:1);
        memcpy(short_pmt,pmt,n);
        assert(psi_build(&s,pat,16,short_pmt,n,115)<0);
        free(short_pmt);
    }
    for (n=0;n<16;n++) {
        uint8_t *short_pat=malloc(n?n:1);
        memcpy(short_pat,pat,n);
        assert(psi_build(&s,short_pat,n,pmt,32,115)<0);
        free(short_pat);
    }
    for (n=0;n<32;n++) {
        uint8_t bad[32]; memcpy(bad,pmt,32); bad[n]^=1;
        assert(psi_build(&s,pat,16,bad,32,115)<0);
    }
    pmt[25]=32; psi_finish(pmt,28); /* duplicate ES PID */
    assert(psi_build(&s,pat,16,pmt,32,115)<0);
    pmt[25]=33; pmt[13]=3; psi_finish(pmt,28); /* too-short CA descriptor */
    assert(psi_build(&s,pat,16,pmt,32,115)<0);
    pmt[13]=4; pmt[7]=1; psi_finish(pmt,28); /* unsupported multi-section PMT */
    assert(psi_build(&s,pat,16,pmt,32,115)<0);
    puts("service PSI: CRC, boundaries, CA descriptors and single-program tables passed");
    return 0;
}
