// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CRYSTAL_H
#define CRYSTAL_H

#include "goxel.h"
#include <stdbool.h>
#include <stddef.h>

#define CRYSTAL_MAX_STATE (20 * 1024 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

void crystal_set_helper(const char *path);
bool crystal_load(const char *path, bool frame);
bool crystal_active(void);
void crystal_reset(void);
void crystal_render(renderer_t *rend, bool preview);
bool crystal_pick(const camera_t *camera,
                  const float view[4],
                  const float pos[2],
                  float out[3],
                  float normal[3]);
bool crystal_prepare_edit(const float box[4][4]);
void crystal_follow_view(const camera_t *camera);
bool crystal_reference_bounds(float box[4][4]);
void crystal_panel(void);
void crystal_show_panel(void);
bool crystal_save_state(char **data, size_t *size);
uint32_t crystal_project_key(void);
void crystal_restore_state(const char *data, size_t size);
int crystal_smoke(const char *output);

#ifdef __cplusplus
}
#endif

#endif
