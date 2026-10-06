/* Test the driver's real parser with malformed and truncated USB replies. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../driver/smit_transport_codec.h"
#include "../driver/smit_ci_codec.h"
#include "../driver/smit_ca_pmt.h"
int main(void)
{
    unsigned char ack[] = {1, 0, 0x80, 2, 1, 0x80};
    unsigned char tail[] = {1, 0, 0x81, 1, 0x42, 0x80, 0, 0, 0x80};
    int n, length;
    unsigned char sas[] = {1, 0x90, 2, 0, 3, 0x9f, 0x9a, 7, 8, 255, 0, 5, 0, 0x11, 0, 1, 'x'};
    assert(smit_sas_reply(sas, sizeof(sas), 0x11) == 16);
    assert(smit_sas_reply(sas, sizeof(sas), 0x22) == -EPROTO);
    for (n = 0; n < (int)sizeof(sas); n++) {
        unsigned char *p = malloc(n ? n : 1);
        memcpy(p, sas, n);
        assert(smit_sas_reply(p, n, 0x11) == -EPROTO);
        free(p);
    }
    for (n = 0; n <= 255; n++) {
        unsigned char p[17];
        memcpy(p, sas, sizeof(p));
        p[8] = n;
        assert((smit_sas_reply(p, sizeof(p), 0x11) >= 0) == (n == 8));
    }
    sas[11] = 6;
    assert(smit_sas_reply(sas, sizeof(sas), 0x11) == -EPROTO);
    unsigned char capmt[] = {0x9f, 0x80, 0x32, 13,   3,  0,    115, 0xc1, 0xf0,
                             1,    1,    0x1b, 0xe0, 32, 0xf0, 1,   1};
    assert(smit_ca_pmt_valid(capmt, sizeof(capmt)));
    {
        unsigned char stop[250], encoded[260], payload[255] = {0};
        int size = smit_ca_stop_candidate(capmt, sizeof(capmt), stop, sizeof(stop));
        assert(size == 15 && smit_ca_pmt_valid(stop, size));
        assert(stop[4] == 3 && stop[6] == 115 && stop[9] == 0);
        assert(stop[10] == 0x1b && stop[12] == 32 && stop[14] == 0);
        assert(smit_ca_stop_candidate(capmt, sizeof(capmt), stop, 14) == -EMSGSIZE);
        for (n = 0; n <= 255; n++) {
            size = smit_wire_encode(encoded, sizeof(encoded), 0xa0, payload, n);
            assert(size == n + (n < 128 ? 4 : 5));
            assert(smit_wire_length(encoded, size) == n);
            assert(smit_wire_encode(encoded, size - 1, 0xa0, payload, n) == -EMSGSIZE);
        }
        assert(smit_wire_encode(encoded, sizeof(encoded), 0xa0, payload, 256) == -EMSGSIZE);
        for (n = 0; n < (int)sizeof(capmt); n++)
            assert(smit_ca_stop_candidate(capmt, n, stop, sizeof(stop)) == -EINVAL);
    }
    for (n = 0; n < (int)sizeof(capmt); n++) {
        unsigned char *p = malloc(n ? n : 1);
        memcpy(p, capmt, n);
        assert(!smit_ca_pmt_valid(p, n));
        free(p);
    }
    capmt[9] = 255;
    assert(!smit_ca_pmt_valid(capmt, sizeof(capmt)));
    capmt[9] = 1;
    capmt[15] = 255;
    assert(!smit_ca_pmt_valid(capmt, sizeof(capmt)));
    assert(smit_wire_length(NULL, 0) == -EPROTO);
    assert(smit_wire_length(ack, 6) == 2);
    assert(smit_wire_complete(ack, 6));
    assert(!smit_wire_complete(ack, 5));
    assert(smit_wire_complete(tail, 9));
    assert(!smit_wire_complete(tail, 8));
    {
        unsigned char large[170] = {1, 0, 0xa0, 0x81, 161};
        large[166] = 0x80;
        large[169] = 0x80;
        assert(smit_wire_offset(large, sizeof(large)) == 5);
        assert(smit_wire_length(large, sizeof(large)) == 161);
        assert(smit_wire_length(large, 165) == -EMSGSIZE);
        assert(smit_wire_complete(large, sizeof(large)));
        assert(!smit_wire_complete(large, 169));
        assert(smit_wire_length(large, 4) == -EMSGSIZE);
        large[3] = 0x80;
        assert(smit_wire_length(large, sizeof(large)) == -EPROTO);
    }
    /* Exact-size allocations make ASan catch every read past actual bytes. */
    for (n = 0; n <= 263; n++) {
        unsigned char *p = malloc(n ? n : 1);
        for (length = 0; length <= 255; length++) {
            memset(p, 0x80, n);
            if (n)
                p[0] = 1;
            if (n >= 4)
                p[3] = length;
            if (length < 128)
                assert(smit_wire_length(p, n) == (n < 4            ? -EPROTO
                                                  : length > n - 4 ? -EMSGSIZE
                                                                   : length));
            else
                (void)smit_wire_length(p, n);
            (void)smit_wire_complete(p, n);
            (void)smit_ca_pmt_valid(p, n);
            if (n) {
                p[0] = 0xfe;
                assert(smit_wire_length(p, n) == -EPROTO);
            }
        }
        free(p);
    }
    puts("wire parser: boundary and truncation checks passed");
    return 0;
}
