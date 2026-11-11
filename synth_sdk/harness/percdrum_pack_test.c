
#include "../plugins/percdrum/perc_drum.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6*0x24]; int16_t host_voice_vel[6];
int main(void){
    srand(9);
    /* fill slot 0 with a mix of built-in and transferred keys */
    for(int pq=0;pq<PD_PADS;pq++){
        pd_key *k=&kit[0][pq]; key_default(k,pq);
        if(pq%3==0){ k->sound=(uint8_t)(PERC_FACTORY_COUNT+ (pq%16)); for(int j=0;j<21;j++){k->patch[j]=(uint16_t)rand();k->right[j]=(pq%2)?(uint16_t)rand():k->patch[j];} }
        k->pitch=(int16_t)(rand()%32768); k->length=(int16_t)(rand()%32768); k->morph=(int16_t)(rand()%32768);
    }
    pd_key saved[PD_PADS]; memcpy(saved,kit[0],sizeof saved);
    unsigned n=pack_kit(0); b64_enc(pd_raw,n);
    printf("packed %u bytes -> %zu base64 chars (%.1f KB kit)\n", n, strlen(pd_b64), strlen(pd_b64)/1024.0);
    /* worst case: all transferred both-sides */
    for(int pq=0;pq<PD_PADS;pq++){ pd_key *k=&kit[0][pq]; k->sound=(uint8_t)(PERC_FACTORY_COUNT+(pq%16)); for(int j=0;j<21;j++){k->patch[j]=(uint16_t)rand();k->right[j]=(uint16_t)rand();} }
    unsigned nw=pack_kit(0); b64_enc(pd_raw,nw);
    printf("worst case (24 unique, both sides): %u bytes -> %zu base64 chars (%.1f KB)\n", nw, strlen(pd_b64), strlen(pd_b64)/1024.0);
    /* round-trip the mixed kit */
    memcpy(kit[0],saved,sizeof saved);
    n=pack_kit(0); b64_enc(pd_raw,n);
    uint8_t raw2[PD_RAW_MAX]; unsigned n2=b64_dec(pd_b64,raw2,PD_RAW_MAX);
    for(int pq=0;pq<PD_PADS;pq++) key_default(&kit[0][pq],pq);
    unpack_kit(0,raw2,n2);
    int bad=0;
    for(int pq=0;pq<PD_PADS;pq++){ pd_key *a=&saved[pq],*b=&kit[0][pq];
        if(a->sound!=b->sound||a->pitch!=b->pitch||a->length!=b->length||a->punch!=b->punch||a->tone!=b->tone||a->drive!=b->drive||a->level!=b->level||a->morph!=b->morph) bad++;
        for(int j=0;j<21;j++) if(a->patch[j]!=b->patch[j]||a->right[j]!=b->right[j]) {bad++;break;} }
    printf("round-trip: %d keys differ %s\n", bad, bad?"FAIL":"OK");
    return bad!=0;
}
