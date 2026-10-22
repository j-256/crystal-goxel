/* Goxel 3D voxels editor
 *
 * copyright (c) 2016 Guillaume Chereau <guillaume@noctua-software.com>
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

/*
 * Support for KVX format, used by the Build engine (Shadow Warrior/Blood)
 * and KV6 format, used by Voxlap, Evaldraw.
 * From the great Ken Silvemans.
 */

#include "goxel.h"
#include "file_format.h"
#include <limits.h>

/*
 * Structure that represents a single voxel and visible faces.
 * Used as an intermediate structure for export.
 */
typedef struct {
    int pos[3];
    int color;
    int vis;
} voxel_t;

/*
 * Structure that represents a single slab, for export.
 */
typedef struct {
    int     pos[3];         // Starting top position.
    uint8_t len;            // Number of voxels in the slab.
    uint8_t vis;            // Visible faces.
    uint8_t colors[256];    // Colors from top to bottom.
} slab_t;


#define READ(type, file) \
    ({ type v; if (fread(&v, sizeof(v), 1, file) != 1) \
        raise("Read file error"); v;})
#define WRITE(type, v, file) \
    do { type v_ = v; if (fwrite(&v_, sizeof(v_), 1, file) != 1) \
        raise("Write file error"); } while (0)

#define raise(msg) do { \
        LOG_E(msg); \
        ret = -1; \
        goto end; \
    } while (0)

static inline int AT(int x, int y, int z, int w, int h, int d) {
    y = h - y - 1;
    z = d - z - 1;
    return x + y * w + z * w * h;
}

static void swap_color(uint32_t v, uint8_t ret[4])
{
    uint8_t o[4];
    memcpy(o, &v, 4);
    ret[0] = o[2];
    ret[1] = o[1];
    ret[2] = o[0];
    ret[3] = o[3];
}

static int kv6_import(const file_format_t *format, image_t *image,
                      const char *path)
{
    FILE *file;
    char magic[4];
    int i, r, ret = 0, w, h, d, blklen, x, y, z = 0, nb, p = 0;
    uint32_t width, height, depth, block_count;
    size_t cube_size;
    uint32_t *xoffsets = NULL;
    uint16_t *xyoffsets = NULL;
    uint8_t (*cube)[4] = NULL;
    uint8_t color[4] = {0};
    struct {
        uint32_t color;
        uint16_t zpos;
        uint8_t visface;
    } *blocks = NULL;

    file = fopen(path, "rb");
    if (!file) return -1;
    r = fread(magic, 1, 4, file);
    if (r != 4 || strncmp(magic, "Kvxl", 4) != 0)
        raise("Invalid magic");
    width = READ(uint32_t, file);
    height = READ(uint32_t, file);
    depth = READ(uint32_t, file);
    if (!width || !height || !depth ||
        width > INT_MAX || height > INT_MAX || depth > INT_MAX ||
        width > INT_MAX / height ||
        width * height > INT_MAX / depth)
        raise("Invalid KV6 dimensions");
    w = width;
    h = height;
    d = depth;
    cube_size = (size_t)w * h * d;
    if (cube_size > SIZE_MAX / sizeof(*cube))
        raise("KV6 voxel buffer too large");
    cube = calloc(cube_size, sizeof(*cube));
    if (!cube) raise("Cannot allocate KV6 voxel buffer");

    READ(float, file);
    READ(float, file);
    READ(float, file);
    block_count = READ(uint32_t, file);
    if (block_count > INT_MAX ||
        (block_count && sizeof(*blocks) > SIZE_MAX / block_count))
        raise("Invalid KV6 block count");
    blklen = block_count;
    if (blklen) {
        blocks = calloc(blklen, sizeof(*blocks));
        if (!blocks) raise("Cannot allocate KV6 blocks");
    }
    for (i = 0; i < blklen; i++) {
        blocks[i].color = READ(uint32_t, file);
        blocks[i].zpos = READ(uint16_t, file);
        blocks[i].visface = READ(uint8_t, file);
        READ(uint8_t, file); // lighting
    }
    xoffsets = calloc(w, sizeof(*xoffsets));
    xyoffsets = calloc(w * h, sizeof(*xyoffsets));
    if (!xoffsets || !xyoffsets) raise("Cannot allocate KV6 offsets");
    for (i = 0; i < w; i++)      xoffsets[i] = READ(uint32_t, file);
    for (i = 0; i < w * h; i++) xyoffsets[i] = READ(uint16_t, file);

    for (x = 0; x < w; x++)
    for (y = 0; y < h; y++) {
        nb = xyoffsets[x * h + y];
        if (nb > blklen - p) raise("KV6 block count exceeds data");
        for (i = 0; i < nb; i++, p++) {
            z = blocks[p].zpos;
            if (z >= d) raise("KV6 voxel outside dimensions");
            swap_color(blocks[p].color, cube[AT(x, y, z, w, h, d)]);
        }
    }

    // Fill
    p = 0;
    for (x = 0; x < w; x++)
    for (y = 0; y < h; y++) {
        nb = xyoffsets[x * h + y];
        z = 0;
        for (i = 0; i < nb; i++, p++) {
            if (blocks[p].visface & 0x10) {
                z = blocks[p].zpos;
                swap_color(blocks[p].color, color);
                color[3] = 255;
            }
            if (blocks[p].visface & 0x20) {
                for (; z < blocks[p].zpos; z++)
                    if (cube[AT(x, y, z, w, h, d)][3] == 0)
                        memcpy(cube[AT(x, y, z, w, h, d)], color, 4);
            }
        }
    }

    volume_blit(image->active_layer->volume, (const uint8_t*)cube,
              -w / 2, -h / 2, -d / 2, w, h, d, NULL);
end:
    free(cube);
    free(blocks);
    free(xoffsets);
    free(xyoffsets);
    fclose(file);
    return ret;
}

static int kvx_import(const file_format_t *format, image_t *image,
                      const char *path)
{
    FILE *file;
    int i, ret = 0, nb, lastz = 0, len, visface;
    int w, h, d, px, py, pz, x, y, z;
    uint32_t file_size, width, height, depth;
    size_t cube_size;
    uint64_t offset_bytes;
    int aabb[2][3];
    uint8_t color = 0;
    uint8_t (*palette)[4] = NULL;
    uint32_t *xoffsets = NULL;
    uint16_t *xyoffsets = NULL;
    uint8_t (*cube)[4] = NULL;
    long datpos, palette_pos, slab_pos;

    path = path ?: sys_open_file_dialog("Open", NULL, format->exts,
                                        format->exts_desc);
    if (!path) return -1;

    file = fopen(path, "rb");
    if (!file) return -1;
    file_size = READ(uint32_t, file);
    width = READ(uint32_t, file);
    height = READ(uint32_t, file);
    depth = READ(uint32_t, file);
    if (!width || !height || !depth ||
        width >= INT_MAX || height >= INT_MAX || depth > INT_MAX ||
        width > INT_MAX / height ||
        width * height > INT_MAX / depth ||
        width > INT_MAX / (height + 1))
        raise("Invalid KVX dimensions");
    w = width;
    h = height;
    d = depth;
    cube_size = (size_t)w * h * d;
    if (cube_size > SIZE_MAX / sizeof(*cube))
        raise("KVX voxel buffer too large");
    cube = calloc(cube_size, sizeof(*cube));
    if (!cube) raise("Cannot allocate KVX voxel buffer");

    px = READ(uint32_t, file) / 256;
    py = READ(uint32_t, file) / 256;
    pz = READ(uint32_t, file) / 256;

    xoffsets = calloc((size_t)w + 1, sizeof(*xoffsets));
    xyoffsets = calloc((size_t)w * (h + 1), sizeof(*xyoffsets));
    if (!xoffsets || !xyoffsets) raise("Cannot allocate KVX offsets");
    for (i = 0; i < w + 1; i++)        xoffsets[i] = READ(uint32_t, file);
    for (i = 0; i < w * (h + 1); i++) xyoffsets[i] = READ(uint16_t, file);

    offset_bytes = ((uint64_t)w + 1) * 4 +
                   (uint64_t)w * (h + 1) * 2;
    if (file_size < 24 || offset_bytes > file_size - 24)
        raise("Invalid KVX offsets");
    if (xoffsets[w] != file_size - 24) LOG_W("Invalid KVX final offset");

    datpos = ftell(file);
    if (datpos < 0) raise("Cannot locate KVX voxel data");

    // Read the palette at the end of the file first.
    if (fseek(file, -256 * 3, SEEK_END) != 0)
        raise("Cannot locate KVX palette");
    palette_pos = ftell(file);
    if (palette_pos < datpos) raise("Invalid KVX data length");
    palette = calloc(256, sizeof(*palette));
    if (!palette) raise("Cannot allocate KVX palette");
    for (i = 0; i < 256; i++) {
        palette[i][0] = clamp(round(READ(uint8_t, file) * 255 / 63.f), 0, 255);
        palette[i][1] = clamp(round(READ(uint8_t, file) * 255 / 63.f), 0, 255);
        palette[i][2] = clamp(round(READ(uint8_t, file) * 255 / 63.f), 0, 255);
        palette[i][3] = 255;
    }
    if (fseek(file, datpos, SEEK_SET) != 0)
        raise("Cannot return to KVX voxel data");

    for (x = 0; x < w; x++)
    for (y = 0; y < h; y++) {
        lastz = 0;
        if (xyoffsets[x * (h + 1) + y + 1] < xyoffsets[x * (h + 1) + y])
            raise("Invalid format");
        nb = xyoffsets[x * (h + 1) + y + 1] - xyoffsets[x * (h + 1) + y];
        slab_pos = ftell(file);
        if (slab_pos < 0 || nb > palette_pos - slab_pos)
            raise("KVX slab exceeds voxel data");
        while (nb > 0) {
            if (nb < 3) raise("Invalid KVX slab length");
            z = READ(uint8_t, file);
            len = READ(uint8_t, file);
            visface = READ(uint8_t, file);
            if (!len || len > nb - 3 || z + len > d)
                raise("Invalid KVX voxel span");
            for (i = 0; i < len; i++) {
                color = READ(uint8_t, file);
                memcpy(cube[AT(x, y, z + i, w, h, d)], palette[color], 4);
            }
            nb -= len + 3;

            /* KVX format only saves the visible voxels.  Since we have the
             * face information, we can fill the gaps ourself between
             * top visible and bottom visible voxels.
             * Note: this should be an option.  */
            if (visface & 0x10) lastz = z + len;
            if (visface & 0x20) {
                for (i = lastz; i < z; i++) {
                    if (cube[AT(x, y, i, w, h, d)][3] == 0) {
                        memcpy(cube[AT(x, y, i, w, h, d)], palette[color], 4);
                    }
                }
            }
        }
    }

    vec3_set(aabb[0], -px, -py, pz - d);
    vec3_set(aabb[1], w - px, h - py, pz);

    bbox_from_aabb(image->box, aabb);
    bbox_from_aabb(image->active_layer->box, aabb);
    volume_blit(image->active_layer->volume, (uint8_t*)cube,
              -px, -py, pz - d, w, h, d, NULL);

end:
    free(palette);
    free(cube);
    free(xoffsets);
    free(xyoffsets);
    fclose(file);
    return ret;
}


static int get_color_index(uint8_t v[4], uint8_t (*palette)[4], bool exact)
{
    const uint8_t *c;
    int i, dist, best = -1, best_dist = 1024;
    for (i = 1; i < 255; i++) {
        c = palette[i];
        dist = abs((int)c[0] - (int)v[0]) +
               abs((int)c[1] - (int)v[1]) +
               abs((int)c[2] - (int)v[2]);
        if (dist == 0) return i;
        if (exact) continue;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

// Sort the voxels as they appear in the slabs.
static int voxel_cmp(const void *a_, const void *b_)
{
    const voxel_t *a = (void*)a_;
    const voxel_t *b = (void*)b_;
    return cmp(a->pos[0], b->pos[0]) ?:
           cmp(a->pos[1], b->pos[1]) ?:
           cmp(a->pos[2], b->pos[2]);
}

/*
 * Attempt to add a voxel into a slab, return true for success.
 */
static bool slab_append(slab_t *slab, voxel_t *vox)
{
    if (slab->vis & 32) return false; // Slab already finished.
    if (slab->len == 255) return false; // No more space.

    if (slab->len == 0) {
        memcpy(slab->pos, vox->pos, sizeof(slab->pos));
        slab->vis = vox->vis;
    }

    if (    vox->pos[0] != slab->pos[0] ||
            vox->pos[1] != slab->pos[1] ||
            vox->pos[2] != slab->pos[2] + slab->len)
        return false;

    // All the vertical faces should have the same visibility.
    if ((vox->vis & 15) != (slab->vis & 15)) return false;
    slab->vis |= (vox->vis & 32); // Add bottom face if needed.
    slab->colors[slab->len++] = vox->color;
    return true;
}


static int kvx_export(const file_format_t *format, const image_t *image,
                      const char *path)
{
    FILE *file = NULL;
    uint8_t (*palette)[4] = NULL;
    volume_iterator_t iter;
    volume_accessor_t acc;
    uint8_t v[4];
    float box[4][4];
    int pos[3], size[3], orig[3], x, y, i;
    UT_array *slabs = NULL;
    UT_array *voxels = NULL;
    slab_t *slab;
    voxel_t voxel, *vox;
    uint64_t ofs, header_size;
    uint32_t *xoffsets = NULL;
    uint16_t *xyoffsets = NULL;
    size_t xy_count;
    double dimensions[3], origin[3], pivot[3];
    int ret = 0;
    bool use_current_palette = false;
    const volume_t *volume = goxel_get_layers_volume(image);

    UT_icd voxel_icd = {sizeof(voxel_t), NULL, NULL, NULL};
    UT_icd slab_icd = {sizeof(slab_t), NULL, NULL, NULL};

    mat4_copy(image->box, box);
    if (box_is_null(box)) volume_get_box(volume, true, box);

    if (!path) return -1;
    for (i = 0; i < 3; i++) {
        dimensions[i] = (double)box[i][i] * 2;
        origin[i] = (double)box[3][i] - box[i][i];
        if (!(dimensions[i] >= 1 && dimensions[i] < INT_MAX &&
              origin[i] >= INT_MIN &&
              origin[i] <= INT_MAX - dimensions[i] + 1)) return -1;
        size[i] = dimensions[i];
        orig[i] = origin[i];
    }
    if (size[2] > 256) return -1;
    pivot[0] = -origin[0] + (size[0] % 2) / 2.;
    pivot[1] = -origin[1] + (size[1] % 2) / 2.;
    pivot[2] = size[2] - origin[2];
    for (i = 0; i < 3; i++) {
        if (!(pivot[i] * 256 >= INT32_MIN &&
              pivot[i] * 256 <= INT32_MAX)) return -1;
    }
    header_size = ((uint64_t)size[0] + 1) * sizeof(*xoffsets) +
                  (uint64_t)size[0] * (size[1] + 1) * sizeof(*xyoffsets);
    if (header_size > UINT32_MAX - 24) return -1;
    xy_count = (size_t)size[0] * (size[1] + 1);
    if (xy_count > SIZE_MAX / sizeof(*xyoffsets)) return -1;

    // If the current palette is enough to export the model, use it, else
    // create a palette.
    if (goxel.palette->size == 256) {
        use_current_palette = true;
        iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
        while (volume_iter(&iter, pos)) {
            volume_get_at(volume, &iter, pos, v);
            if (v[3] < 127) continue;
            if (palette_search(goxel.palette, v, true) < 0) {
                use_current_palette = false;
                break;
            }
        }
    }
    palette = calloc(256, sizeof(*palette));
    if (!palette) return -1;
    if (use_current_palette) {
        LOG_I("Using the current palette");
        for (i = 0; i < 256; i++) {
            memcpy(palette[i], goxel.palette->entries[i].color, 4);
        }
    } else {
        quantization_gen_palette(volume, 256, (void*)(palette));
    }

    // Iter the voxels and only keep the visible ones, plus the visible
    // faces mask.  Put them all into an array.
    utarray_new(voxels, &voxel_icd);
    iter = volume_get_iterator(volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    acc = volume_get_accessor(volume);

    while (volume_iter(&iter, voxel.pos)) {
        volume_get_at(volume, &iter, voxel.pos, v);
        if (v[3] < 127) continue;

        // XXX: we should be able to use box iterator instead of this,
        // but for some reason it doesn't work!
        if (!bbox_contains_vec(box,
                    (float[]){voxel.pos[0], voxel.pos[1], voxel.pos[2]}))
            continue;

        // Compute visible face mask.
        voxel.vis = 0;
        #define vis_test(x, y, z) \
            (volume_get_alpha_at(volume, &acc, \
                        (int[]){voxel.pos[0] + (x), \
                                voxel.pos[1] + (y), \
                                voxel.pos[2] + (z)}) < 127)
        if (vis_test(-1,  0,  0)) voxel.vis |= 1;
        if (vis_test(+1,  0,  0)) voxel.vis |= 2;
        if (vis_test( 0, +1,  0)) voxel.vis |= 4;
        if (vis_test( 0, -1,  0)) voxel.vis |= 8;
        if (vis_test( 0,  0, +1)) voxel.vis |= 16;
        if (vis_test( 0,  0, -1)) voxel.vis |= 32;

        #undef vis_test
        if (!voxel.vis) continue; // No visible faces.
        voxel.color = get_color_index(v, palette, false);
        voxel.pos[0] -= orig[0];
        voxel.pos[1] -= orig[1];
        voxel.pos[2] -= orig[2];

        voxel.pos[1] = size[1] - voxel.pos[1] - 1;
        voxel.pos[2] = size[2] - voxel.pos[2] - 1;

        assert(voxel.pos[0] >= 0 && voxel.pos[0] < size[0]);
        assert(voxel.pos[1] >= 0 && voxel.pos[1] < size[1]);
        assert(voxel.pos[2] >= 0 && voxel.pos[2] < size[2]);
        utarray_push_back(voxels, &voxel);
    }

    // Sort the voxels by xy columns in order they will be in the slabs.
    utarray_sort(voxels, voxel_cmp);

    // Iter the voxels and generates the slabs array.
    utarray_new(slabs, &slab_icd);
    utarray_extend_back(slabs); // Add an initial slab.
    for (vox = (void*)utarray_front(voxels); vox;
         vox = (void*)utarray_next(voxels, vox))
    {
        slab = (void*)utarray_back(slabs);
        if (slab_append(slab, vox)) continue;
        // Finished a slab, create a new one.
        utarray_extend_back(slabs); // Add a new slab.
        slab = (void*)utarray_back(slabs);
        slab_append(slab, vox);     // Always works.
    }

    // Compute xoffsets and xyoffsetx.
    // Can we do it in a simpler way?
    xoffsets = calloc((size_t)size[0] + 1, sizeof(*xoffsets));
    xyoffsets = calloc(xy_count, sizeof(*xyoffsets));
    if (!xoffsets || !xyoffsets) raise("Cannot allocate KVX offsets");
    ofs = header_size;
    xoffsets[0] = ofs;

    slab = (void*)utarray_front(slabs);
    for (x = 0; x < size[0]; x++) {
        while (slab && slab->pos[0] <= x) {
            if (ofs > UINT32_MAX - 24 - (3 + slab->len))
                raise("KVX voxel data too large");
            ofs += 3 + slab->len;
            slab = (void*)utarray_next(slabs, slab);
        }
        xoffsets[x + 1] = ofs;
    }

    ofs = header_size;
    slab = (void*)utarray_front(slabs);
    for (x = 0; x < size[0]; x++) {
        xyoffsets[(size_t)x * (size[1] + 1)] = 0;
        for (y = 0; y < size[1]; y++) {
            while (slab && slab->pos[0] <= x && slab->pos[1] <= y) {
                ofs += 3 + slab->len;
                slab = (void*)utarray_next(slabs, slab);
            }
            if (ofs - xoffsets[x] > UINT16_MAX)
                raise("KVX column offsets too large");
            xyoffsets[(size_t)x * (size[1] + 1) + y + 1] =
                ofs - xoffsets[x];
        }
    }

    // Now we have all the data ready to be saved.
    // The byte size is the last x offset plus the header size (24).
    file = fopen(path, "wb");
    if (!file) raise("Cannot open KVX file");
    WRITE(uint32_t, xoffsets[size[0]] + 24, file);
    WRITE(uint32_t, size[0], file);
    WRITE(uint32_t, size[1], file);
    WRITE(uint32_t, size[2], file);

    WRITE(int32_t, (int)(pivot[0] * 256), file);
    WRITE(int32_t, (int)(pivot[1] * 256), file);
    WRITE(int32_t, (int)(pivot[2] * 256), file);

    for (i = 0; i < size[0] + 1; i++)
        WRITE(uint32_t, xoffsets[i], file);
    for (i = 0; (size_t)i < xy_count; i++)
        WRITE(uint16_t, xyoffsets[i], file);

    for (slab = (void*)utarray_front(slabs); slab;
         slab = (void*)utarray_next(slabs, slab))
    {
        assert(slab->pos[2] >= 0);
        assert(slab->pos[2] <= 255);
        assert(slab->pos[2] + slab->len - 1 < size[2]);
        WRITE(uint8_t, slab->pos[2], file);
        WRITE(uint8_t, slab->len, file);
        WRITE(uint8_t, slab->vis, file);
        if (fwrite(slab->colors, 1, slab->len, file) != slab->len)
            raise("Write KVX slab error");
    }

    for (i = 0; i < 256; i++) {
        WRITE(uint8_t, palette[i][0] / 4, file);
        WRITE(uint8_t, palette[i][1] / 4, file);
        WRITE(uint8_t, palette[i][2] / 4, file);
    }

end:
    if (slabs) utarray_free(slabs);
    if (voxels) utarray_free(voxels);
    free(xoffsets);
    free(xyoffsets);
    free(palette);
    if (file && fclose(file) != 0) ret = -1;
    if (ret && file) sys_delete_file(path);
    return ret;
}

FILE_FORMAT_REGISTER(kv6,
    .name = "kv6",
    .exts = {"*.kv6"},
    .exts_desc = "slab",
    .import_func = kv6_import,
)

FILE_FORMAT_REGISTER(kvx,
    .name = "kvx",
    .exts = {"*.kvx"},
    .exts_desc = "slab",
    .import_func = kvx_import,
    .export_func = kvx_export,
)
