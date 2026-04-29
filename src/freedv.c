/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI — FreeDV PoC scaffold (ki9ng fork)
 *
 *  Phase 0 (v0.1) — scaffold-only.
 *
 *  This file deliberately does NOT include <codec2/freedv_api.h> yet. codec2
 *  is vendored as a git submodule at third-party/codec2 (tag 1.2.0) so the
 *  source is in tree, but the buildroot package set does not yet know how to
 *  cross-compile it for the A33 sysroot. Wiring codec2 into the build is the
 *  v0.2 milestone.
 *
 *  In v0.1 the only behaviour is:
 *    - print a banner to stderr at init time
 *    - hold the current mode value in module-static state
 *
 *  This is enough to prove the SD image builds, boots, and identifies itself
 *  as the FreeDV-only PoC variant before we take on the higher-risk audio
 *  bridge work.
 */

#include "freedv.h"

#include <stdio.h>

#define FREEDV_POC_VERSION "v0.1"

static freedv_mode_t current_mode = FREEDV_MODE_OFF;
static bool initialised = false;

void freedv_init(void) {
    if (initialised) return;
    initialised = true;
    current_mode = FREEDV_MODE_OFF;

    fprintf(stderr,
        "[freedv] FreeDV-only PoC %s — scaffold init (codec2 not yet linked)\n",
        FREEDV_POC_VERSION);
    fprintf(stderr,
        "[freedv] swap to a normal r1cbu/gdyuldin SD image to use this radio "
        "for non-FreeDV operation.\n");
}

void freedv_deinit(void) {
    if (!initialised) return;
    initialised = false;
    current_mode = FREEDV_MODE_OFF;
    fprintf(stderr, "[freedv] deinit\n");
}

freedv_mode_t freedv_get_mode(void) {
    return current_mode;
}

void freedv_set_mode(freedv_mode_t mode) {
    if (mode == current_mode) return;
    fprintf(stderr, "[freedv] mode %s -> %s\n",
        freedv_mode_label(current_mode), freedv_mode_label(mode));
    current_mode = mode;
}

const char *freedv_mode_label(freedv_mode_t mode) {
    switch (mode) {
        case FREEDV_MODE_OFF:  return "OFF";
        case FREEDV_MODE_700D: return "700D";
        case FREEDV_MODE_1600: return "1600";
        default:               return "?";
    }
}

bool freedv_is_active(void) {
    return initialised && current_mode != FREEDV_MODE_OFF;
}
