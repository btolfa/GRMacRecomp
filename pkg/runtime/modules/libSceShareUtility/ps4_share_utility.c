// SPDX-License-Identifier: GPL-2.0-or-later
// libSceShareUtility: the system share menu (upload a screenshot or a video clip).
//
// The calls succeed as on a console. The share menu itself is a system overlay the host does not
// have: an open request is logged and shows nothing. The game gets no result from the menu, so
// it continues as if the user closed it.
#include "recomp_runtime.h"

#include <stdio.h>

void shim_sceShareUtilityInitialize(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_sceShareUtilityTerminate(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_sceShareUtilityOpenShareMenu(GuestContext *ctx) {
  fprintf(stderr, "[share] share menu requested; the host has no share menu\n");
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_sceShareUtilityOpenShareMenuDefault(GuestContext *ctx) {
  fprintf(stderr, "[share] share menu requested; the host has no share menu\n");
  ctx->rax = 0;
  SHIM_RETURN();
}
