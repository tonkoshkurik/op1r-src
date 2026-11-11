/* percdrum morph lerp, host only (from synth_sdk):
 *   cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DOP1_HOST -I. -Iharness -Iplugins/percsyn \
 *      harness/percdrum_morph_test.c -o harness/build/percdrum_morph_test && harness/build/percdrum_morph_test */
#include "../plugins/percdrum/perc_drum.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int main(void)
{
    pd_key k; uint16_t t[21]; srand(5);
    for (int r = 0; r < 100000; r++) {
        for (int i = 0; i < 21; i++) { k.patch[i] = (uint16_t)rand(); k.right[i] = (uint16_t)rand(); }
        k.morph = 0; assert(key_patch(&k, t) == k.patch);
        k.morph = 32767; const uint16_t *p = key_patch(&k, t); for (int i = 0; i < 21; i++) assert(p[i] == k.right[i]);
        k.morph = (int16_t)(rand() & 32767); p = key_patch(&k, t);
        for (int i = 0; i < 21; i++) {
            double m = (k.morph + (k.morph >> 14)) / 32768.0, want = k.patch[i] + (k.right[i] - (double)k.patch[i]) * m;
            assert(p[i] >= (want < 1 ? 0 : want - 1) && p[i] <= want + 1);
        }
    }
    key_default(&k, 0); assert(!key_sided(&k) && k.morph == 0);
    printf("morph lerp: ok (left at 0, right at full, within 1 word between)\n");
    return 0;
}
