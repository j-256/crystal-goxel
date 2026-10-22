/* Goxel 3D voxels editor
 *
 * copyright (c) 2017 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"
#include "file_format.h"
#include <limits.h>

static int export_as_png_slices(const file_format_t *format,
                                const image_t *image, const char *path)
{
    float box[4][4];
    const volume_t *volume;
    int x, y, z, w, h, d, pos[3], start_pos[3];
    double dimensions[3], origin[3];
    size_t stride, pixel_count, pixel_index;
    uint8_t c[4];
    uint8_t *img;
    volume_iterator_t iter = {0};

    volume = goxel_get_layers_volume(image);
    if (!path) return -1;
    mat4_copy(image->box, box);
    if (box_is_null(box)) volume_get_box(volume, true, box);
    dimensions[0] = (double)box[0][0] * 2;
    dimensions[1] = (double)box[1][1] * 2;
    dimensions[2] = (double)box[2][2] * 2;
    origin[0] = (double)box[3][0] - box[0][0];
    origin[1] = (double)box[3][1] - box[1][1];
    origin[2] = (double)box[3][2] - box[2][2];
    for (x = 0; x < 3; x++) {
        if (!(dimensions[x] >= 1 && dimensions[x] <= INT_MAX &&
              origin[x] >= INT_MIN &&
              origin[x] <= INT_MAX - dimensions[x] + 1)) return -1;
        start_pos[x] = origin[x];
    }
    w = dimensions[0];
    h = dimensions[1];
    d = dimensions[2];
    if ((size_t)w > INT_MAX / (size_t)d) return -1;
    stride = (size_t)w * d;
    if (stride > INT_MAX / 4 / (size_t)h) return -1;
    pixel_count = stride * h;
    img = calloc(pixel_count, 4);
    if (!img) return -1;
    for (z = 0; z < d; z++)
    for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
        pos[0] = x + start_pos[0];
        pos[1] = y + start_pos[1];
        pos[2] = z + start_pos[2];
        volume_get_at(volume, &iter, pos, c);
        pixel_index = (size_t)y * stride + (size_t)z * w + x;
        memcpy(img + pixel_index * 4, c, 4);
    }
    img_write(img, (int)stride, h, 4, path);
    free(img);
    return 0;
}

FILE_FORMAT_REGISTER(png_slices,
    .name = "png slices",
    .exts = {"*.png"},
    .exts_desc = "png",
    .export_func = export_as_png_slices,
)
