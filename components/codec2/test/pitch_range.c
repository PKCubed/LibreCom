/* Does the CODEC2_PITCH_MAX_HZ override actually move the pitch model?
   Builds the same c2const the codec uses, then round-trips F0 through the
   real encode_Wo/decode_Wo quantiser. */
#include "defines.h"
#include "codec2_internal.h"
#include "quantise.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

void *codec2_malloc(size_t n){return malloc(n);}
void *codec2_calloc(size_t a,size_t b){return calloc(a,b);}
void  codec2_free(void *p){free(p);}

C2CONST c2const_create(int Fs, float framelength_s);

int main(void)
{
    C2CONST c = c2const_create(8000, 0.01f);
    printf("p_min=%d p_max=%d  ->  modelled F0 range %.0f - %.0f Hz\n",
           c.p_min, c.p_max, 8000.0/c.p_max, 8000.0/c.p_min);

    printf("\n  %-10s %-12s %s\n", "F0 in", "F0 decoded", "error");
    const float f0s[] = {100, 200, 300, 380, 420, 450, 480};
    for (unsigned i = 0; i < sizeof(f0s)/sizeof(f0s[0]); i++) {
        float Wo  = 2.0f*(float)M_PI*f0s[i]/8000.0f;
        int   idx = encode_Wo(&c, Wo, WO_BITS);
        float out = decode_Wo(&c, idx, WO_BITS) * 8000.0f / (2.0f*(float)M_PI);
        printf("  %-10.0f %-12.1f %+.1f Hz%s\n", f0s[i], out, out - f0s[i],
               (idx == (1<<WO_BITS)-1) ? "   <-- CLAMPED at top of range" : "");
    }
    return 0;
}
