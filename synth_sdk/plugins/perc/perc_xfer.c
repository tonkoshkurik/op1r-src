#pragma GCC optimize ("no-tree-loop-distribute-patterns") /* no implicit libc calls */
/* perc: PO-32 transfer reader (decodes a transfer recorded into the slot's sample). Phase 2: not wired yet.
 * GPL-3.0-or-later with the OP1R firmware permission. */
#include "op1_plugin.h"
#include "perc.h"
const uint16_t *perc_xfer_patch(int instrument) { (void)instrument; return 0; }
void perc_xfer_service(void) {}
