// SPDX-License-Identifier: GPL-3.0-or-later
extern "C" {
#include "crystal.h"
#include "goxel.h"
#include "model3d.h"
#include "xxhash.h"
}
#undef min
#undef max
#undef clamp
#include "../ext_src/json/json-builder.h"
#include "../ext_src/json/json.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
constexpr uint8_t TOKEN_SIGNATURE = 0xc7;
constexpr size_t MAX_ASSET = 256 * 1024 * 1024;
constexpr size_t MAX_STATE = CRYSTAL_MAX_STATE;
constexpr size_t MAX_PREVIEW_VERTICES = 4'000'000;
constexpr char DEFAULT_EXPORT_NAME[] = "crystal-project.json";
using Json = std::unique_ptr<json_value, decltype(&json_value_free)>;
using Model = std::unique_ptr<model3d_t, decltype(&model3d_delete)>;
struct Block {
    int id, max_variant;
    std::string name;
};
struct State {
    std::string path, manifest, source, managed = "[]";
    std::array<int, 3> origin{}, size{};
    std::vector<Block> blocks;
    std::map<int, std::vector<model_vertex_t>> templates;
    Model reference{ nullptr, model3d_delete };
    Model authored{ nullptr, model3d_delete };
    Model picking{ nullptr, model3d_delete };
    std::vector<std::array<int, 3>> reference_cells, authored_cells,
            picking_cells;
    texture_t *atlas = nullptr;
    std::vector<uint8_t> pixels;
    int atlas_w = 0, atlas_h = 0;
    uint32_t key = 0;
    uint32_t committed_key = 0;
    uint32_t metadata_key = 0;
    size_t invalid = 0;
    bool show = true;
    bool new_objects_solid = true;
    ~State()
    {
        texture_delete(atlas);
    }
};
std::unique_ptr<State> state;
std::string helper, status, pending;
uint32_t pending_key = 0;
char context_input[4096] = {};
int selected = 0, variant = 0;

std::string read_file(const std::filesystem::path &path,
                      size_t limit = MAX_ASSET)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    input.seekg(0, std::ios::end);
    auto length = input.tellg();
    if (length < 0 || static_cast<size_t>(length) > limit)
        throw std::runtime_error("File exceeds the bridge size limit");
    std::string text(static_cast<size_t>(length), '\0');
    input.seekg(0);
    input.read(text.data(), text.size());
    if (!input && !text.empty())
        throw std::runtime_error("Truncated bridge file");
    return text;
}

Json parse(const std::string &text)
{
    json_settings settings = {};
    settings.value_extra = json_builder_extra;
    settings.max_memory = MAX_STATE * 8;
    Json value(json_parse_ex(&settings, text.data(), text.size(), nullptr),
               json_value_free);
    if (!value || value->type != json_object)
        throw std::runtime_error("Invalid bridge JSON");
    return value;
}

int number(const json_value &value)
{
    // The bundled parser's implicit bool conversion otherwise wins integer
    // casts and turns IDs into zero
    if (value.type != json_integer || value.u.integer < INT_MIN ||
        value.u.integer > INT_MAX)
        throw std::runtime_error("Expected a bounded bridge integer");
    return static_cast<int>(value.u.integer);
}

std::string serialize(const json_value *value)
{
    auto *mutable_value = const_cast<json_value *>(value);
    auto *parent = mutable_value->parent;
    mutable_value->parent = nullptr;
    std::string text(json_measure(mutable_value), '\0');
    json_serialize(text.data(), mutable_value);
    mutable_value->parent = parent;
    text.resize(strlen(text.c_str()));
    return text;
}

std::string quote(const std::string &value)
{
    auto *node = json_string_new_length(value.size(), value.data());
    auto text = serialize(node);
    json_builder_free(node);
    return text;
}

std::string string(const json_value &value)
{
    if (value.type != json_string)
        throw std::runtime_error("Expected a bridge string");
    return std::string(value.u.string.ptr, value.u.string.length);
}

bool new_objects_solid(const json_value &root)
{
    const auto &option = root["newObjectsSolid"];
    // Older saved projects and export snapshots use the construction default
    if (option.type == json_none) return true;
    if (option.type != json_boolean)
        throw std::runtime_error("newObjectsSolid must be boolean");
    return option.u.boolean;
}

void update_metadata_key(State &saved)
{
    saved.metadata_key = 0;
    for (const auto *value :
         { &saved.path, &saved.manifest, &saved.source, &saved.managed })
        saved.metadata_key = XXH32(
                value->data(), value->size(), saved.metadata_key);
}

std::string saved_data(const State &saved)
{
    auto data = "{\"format\":1,\"context\":" + quote(saved.path) +
                ",\"manifest\":" + quote(saved.manifest) +
                ",\"source\":" + quote(saved.source) +
                ",\"managed\":" + saved.managed + ",\"newObjectsSolid\":" +
                (saved.new_objects_solid ? "true}" : "false}");
    if (data.size() > MAX_STATE)
        throw std::runtime_error(
                "Bridge project state exceeds its save limit");
    return data;
}

Json saved_json(const std::string &data)
{
    auto root = parse(data);
    if (number((*root)["format"]) != 1 ||
        (*root)["managed"].type != json_array)
        throw std::runtime_error("Unsupported saved bridge state");
    for (const char *key : { "context", "manifest", "source" })
        string((*root)[key]);
    new_objects_solid(*root);
    return root;
}

std::string run_helper(const std::vector<std::string> &arguments)
{
#ifdef WIN32
    throw std::runtime_error(
            "This prototype's helper launcher requires macOS or Linux");
#else
    if (helper.empty())
        throw std::runtime_error(
                "Set --crystal-helper to the crystal-bridge executable");
    int output[2];
    if (pipe(output))
        throw std::runtime_error("Cannot open the helper output pipe");
    std::vector<char *> argv{ const_cast<char *>(helper.c_str()) };
    for (const auto &arg : arguments)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    auto pid = fork();
    if (pid == 0) {
        close(output[0]);
        dup2(output[1], STDOUT_FILENO);
        dup2(output[1], STDERR_FILENO);
        close(output[1]);
        execv(helper.c_str(), argv.data());
        _exit(127);
    }
    close(output[1]);
    if (pid < 0) {
        close(output[0]);
        throw std::runtime_error("Cannot start helper");
    }
    std::string result;
    char buffer[2048];
    ssize_t length;
    while ((length = read(output[0], buffer, sizeof(buffer))) > 0) {
        if (result.size() < 65536) result.append(buffer, length);
    }
    close(output[0]);
    int code = 0;
    while (waitpid(pid, &code, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(code) || WEXITSTATUS(code) != 0)
        throw std::runtime_error(
                result.empty() ? "Helper failed to start" : result);
    return result;
#endif
}

struct Temporary {
    std::filesystem::path directory;
    Temporary()
    {
#ifdef WIN32
        throw std::runtime_error(
                "Temporary helper workspace is unavailable on this platform");
#else
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "crystal-goxel-ui-XXXXXX")
                                      .string();
        auto *path = mkdtemp(pattern.data());
        if (!path) throw std::runtime_error("Cannot create helper workspace");
        directory = path;
#endif
    }
    ~Temporary()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

Model model(const std::vector<model_vertex_t> &vertices)
{
    Model result(static_cast<model3d_t *>(calloc(1, sizeof(model3d_t))),
                 model3d_delete);
    if (!result) throw std::bad_alloc();
    result->solid = true;
    result->dirty = true;
    result->nb_vertices = vertices.size();
    result->vertices = static_cast<model_vertex_t *>(
            malloc(vertices.size() * sizeof(model_vertex_t)));
    if (!result->vertices && !vertices.empty()) throw std::bad_alloc();
    if (!vertices.empty())
        memcpy(result->vertices, vertices.data(),
               vertices.size() * sizeof(model_vertex_t));
    return result;
}

int32_t integer(const std::string &data, size_t &offset)
{
    if (offset + 4 > data.size())
        throw std::runtime_error("Truncated native mesh");
    int32_t result;
    memcpy(&result, data.data() + offset, 4);
    offset += 4;
    return result;
}

std::vector<model_vertex_t> vertices(
        const std::string &data, size_t &offset, int count)
{
    static_assert(sizeof(model_vertex_t) == 36, "Bridge mesh layout");
    if (count < 0 || count % 3 ||
        static_cast<size_t>(count) >
                (data.size() - offset) / sizeof(model_vertex_t))
        throw std::runtime_error("Invalid native mesh vertex count");
    std::vector<model_vertex_t> result(count);
    if (count)
        memcpy(result.data(), data.data() + offset,
               count * sizeof(model_vertex_t));
    offset += count * sizeof(model_vertex_t);
    for (const auto &v : result) {
        for (float p : v.pos)
            if (!std::isfinite(p) || fabs(p) > 10000)
                throw std::runtime_error("Invalid mesh position");
        for (float p : v.uv)
            if (!std::isfinite(p)) throw std::runtime_error("Invalid mesh UV");
    }
    return result;
}

void choose_block()
{
    if (!state || state->blocks.empty()) return;
    const auto &block = state->blocks.at(selected);
    variant = std::clamp(variant, 0, block.max_variant);
    // Goxel colors are exact identity tokens; rendering resolves them through
    // native block templates
    const uint8_t token[4] = {
        static_cast<uint8_t>(block.id), static_cast<uint8_t>(variant),
        TOKEN_SIGNATURE, 255
    };
    memcpy(goxel.painter.color, token, 4);
    goxel.painter.smoothness = 0;
}

std::string cells_json()
{
    std::ostringstream output;
    output << '[';
    bool first = true;
    std::map<std::array<int, 3>, std::array<uint8_t, 4>> cells;
    for (auto *layer = goxel.image->layers; layer; layer = layer->next) {
        if (layer->base_id || layer->shape || layer->image)
            throw std::runtime_error(
                    "Bake cloned and procedural layers before exporting");
        auto it = volume_get_iterator(layer->volume, VOLUME_ITER_SKIP_EMPTY);
        int p[3];
        uint8_t token[4];
        while (volume_iter(&it, p)) {
            volume_get_at(layer->volume, &it, p, token);
            if (!token[3]) continue;
            if (token[3] != 255 || token[2] != TOKEN_SIGNATURE ||
                !state->templates.count(token[0] * 4 + token[1]))
                throw std::runtime_error(
                        "A cell has no native block identity; repaint it with "
                        "the Crystal palette");
            auto key = std::array<int, 3>{ p[0], p[1], p[2] };
            if (!cells.emplace(key,
                               std::array<uint8_t, 4>{
                                   token[0], token[1], token[2], token[3] })
                         .second)
                throw std::runtime_error("Authored layers overlap; merge them "
                                         "before exporting");
        }
    }
    for (const auto &[p, token] : cells) {
        if (!first) output << ',';
        first = false;
        output << "{\"pos\":[" << p[0] << ',' << p[1] << ',' << p[2]
               << "],\"type\":" << int(token[0])
               << ",\"variant\":" << int(token[1]) << '}';
    }
    output << ']';
    return output.str();
}

void import_project(const char *path)
{
    if (cells_json() != "[]")
        throw std::runtime_error("Import into an empty authored document");
    Temporary temp;
    auto snapshot = temp.directory / "snapshot.json";
    auto report = run_helper({ "import", "--context", state->path, "--source",
                               path, "--output", snapshot.string() });
    auto root = parse(read_file(snapshot, MAX_STATE));
    const auto &cells = (*root)["cells"];
    if (cells.type != json_array)
        throw std::runtime_error("Invalid import snapshot");
    // Check persistence before touching the volume so an oversized import
    // cannot lose its source on save
    State metadata;
    metadata.path = state->path;
    metadata.manifest = state->manifest;
    metadata.source = string((*root)["source"]);
    metadata.managed = serialize(&(*root)["managed"]);
    metadata.new_objects_solid = state->new_objects_solid;
    saved_data(metadata);
    struct Imported {
        std::array<int, 3> pos;
        std::array<uint8_t, 4> token;
    };
    std::vector<Imported> imported;
    for (unsigned i = 0; i < cells.u.array.length; i++) {
        const auto &cell = *cells.u.array.values[i];
        const auto &pos = cell["pos"];
        auto id = number(cell["type"]), v = number(cell["variant"]);
        if (id < 1 || id > 255 || v < 0 || v > 3 ||
            !state->templates.count(id * 4 + v))
            throw std::runtime_error("Invalid imported block identity");
        imported.push_back(
                { { number(pos[0]), number(pos[1]), number(pos[2]) },
                  { uint8_t(id), uint8_t(v), TOKEN_SIGNATURE, 255 } });
    }
    for (const auto &cell : imported)
        volume_set_at(goxel.image->active_layer->volume, nullptr,
                      cell.pos.data(), cell.token.data());
    state->source = std::move(metadata.source);
    state->managed = std::move(metadata.managed);
    update_metadata_key(*state);
    image_history_push(goxel.image);
    status = report;
}

void export_project(const char *path)
{
    Temporary temp;
    auto snapshot = temp.directory / "snapshot.json";
    std::ofstream output(snapshot);
    output << "{\"format\":1,\"source\":" << quote(state->source)
           << ",\"managed\":" << state->managed << ",\"newObjectsSolid\":"
           << (state->new_objects_solid ? "true" : "false")
           << ",\"cells\":" << cells_json() << '}';
    output.close();
    status = run_helper({ "export", "--context", state->path, "--snapshot",
                          snapshot.string(), "--output", path });
}

void export_dialog()
{
    const char *filters[] = { "*.json", nullptr };
    auto *path = sys_get_save_path(
            DEFAULT_EXPORT_NAME, filters, "Crystal Edit JSON");
    if (path) export_project(path);
}

void rebuild(bool preview)
{
    std::vector<model_vertex_t> result;
    std::vector<std::array<int, 3>> cells;
    size_t invalid = 0;
    for (const auto *layer = goxel_get_render_layers(preview); layer;
         layer = layer->next)
    {
        if (!layer->visible || !layer->volume) continue;
        auto it = volume_get_iterator(layer->volume, VOLUME_ITER_SKIP_EMPTY);
        int p[3];
        uint8_t token[4];
        while (volume_iter(&it, p)) {
            volume_get_at(layer->volume, &it, p, token);
            if (!token[3]) continue;
            auto found = state->templates.find(token[0] * 4 + token[1]);
            if (token[2] != TOKEN_SIGNATURE || token[3] != 255 ||
                found == state->templates.end())
            {
                invalid++;
                // Invalid identity stays visible as a red cube and fails
                // export rather than changing its meaning
                Model cube(model3d_cube(), model3d_delete);
                if (result.size() + cube->nb_vertices > MAX_PREVIEW_VERTICES)
                    throw std::runtime_error("Authored mesh exceeds the "
                                             "preview resource limit");
                for (int i = 0; i < cube->nb_vertices; i++) {
                    auto v = cube->vertices[i];
                    for (int k = 0; k < 3; k++)
                        v.pos[k] += p[k];
                    v.color[0] = 255;
                    v.color[1] = 20;
                    v.color[2] = 20;
                    v.uv[0] = v.uv[1] = 0;
                    result.push_back(v);
                    if (i % 3 == 0) cells.push_back({ p[0], p[1], p[2] });
                }
                continue;
            }
            if (result.size() + found->second.size() > MAX_PREVIEW_VERTICES)
                throw std::runtime_error(
                        "Authored mesh exceeds the preview resource limit");
            int index = 0;
            for (auto v : found->second) {
                for (int k = 0; k < 3; k++)
                    v.pos[k] += p[k];
                result.push_back(v);
                if (index++ % 3 == 0) cells.push_back({ p[0], p[1], p[2] });
            }
        }
    }
    // Publish geometry and triangle ownership together so a failed rebuild
    // leaves picking consistent
    state->authored = model(result);
    state->authored_cells = std::move(cells);
    state->invalid = invalid;
}

bool intersect(const model_vertex_t *v,
               const float o[3],
               const float d[3],
               float &distance,
               float &u,
               float &w)
{
    float e1[3], e2[3], h[3], s[3], q[3];
    vec3_sub(v[1].pos, v[0].pos, e1);
    vec3_sub(v[2].pos, v[0].pos, e2);
    vec3_cross(d, e2, h);
    float determinant = vec3_dot(e1, h);
    if (fabs(determinant) < 1e-7) return false;
    vec3_sub(o, v[0].pos, s);
    u = vec3_dot(s, h) / determinant;
    if (u < 0 || u > 1) return false;
    vec3_cross(s, e1, q);
    w = vec3_dot(d, q) / determinant;
    if (w < 0 || u + w > 1) return false;
    distance = vec3_dot(e2, q) / determinant;
    return distance > 0;
}

bool pick_ray(
        const float o[3], const float d[3], float out[3], float normal[3])
{
    float best = INFINITY;
    const model_vertex_t *hit = nullptr;
    std::array<int, 3> owner{};
    auto scan = [&](const model3d_t *mesh,
                    const std::vector<std::array<int, 3>> &cells) {
        if (!mesh) return;
        for (int i = 0; i < mesh->nb_vertices; i += 3) {
            const auto *v = mesh->vertices + i;
            float distance, u, w;
            if (!intersect(v, o, d, distance, u, w) || distance >= best)
                continue;
            float tx = v[0].uv[0] * (1 - u - w) + v[1].uv[0] * u +
                       v[2].uv[0] * w;
            float ty = v[0].uv[1] * (1 - u - w) + v[1].uv[1] * u +
                       v[2].uv[1] * w;
            int x = std::clamp(
                    int(tx * state->atlas_w), 0, state->atlas_w - 1);
            int y = std::clamp(
                    int(ty * state->atlas_h), 0, state->atlas_h - 1);
            if (state->pixels[(y * state->atlas_w + x) * 4 + 3] < 26) continue;
            best = distance;
            hit = v;
            owner = cells.at(i / 3);
        }
    };
    if (state->show) scan(state->reference.get(), state->reference_cells);
    scan(state->picking.get(), state->picking_cells);
    if (!hit) return false;
    vec3_copy(hit->normal, normal);
    // Explicit ownership avoids adjacent-cell errors at triangle edges and
    // inset decorative surfaces
    for (int k = 0; k < 3; k++)
        out[k] = owner[k] + .5f + normal[k] * .5f;
    return true;
}
} // namespace

extern "C" void crystal_set_helper(const char *path)
{
    helper = path ? path : "";
}
extern "C" bool crystal_active(void)
{
    return bool(state);
}
extern "C" void crystal_reset(void)
{
    state.reset();
    pending.clear();
    status.clear();
    context_input[0] = '\0';
    selected = variant = 0;
}

extern "C" bool crystal_load(const char *path, bool frame)
{
    try {
        auto next = std::make_unique<State>();
        next->path = std::filesystem::absolute(path).string();
        next->manifest = read_file(next->path, MAX_STATE);
        auto root = parse(next->manifest);
        run_helper({ "validate", "--context", next->path });
        for (int k = 0; k < 3; k++) {
            next->origin[k] = number((*root)["origin"][k]);
            next->size[k] = number((*root)["size"][k]);
        }
        const auto &blocks = (*root)["blocks"];
        if (blocks.type != json_array || !blocks.u.array.length)
            throw std::runtime_error("Missing native palette");
        for (unsigned i = 0; i < blocks.u.array.length; i++) {
            const auto &b = *blocks.u.array.values[i];
            next->blocks.push_back({ number(b["id"]), number(b["maxVariant"]),
                                     string(b["name"]) });
        }
        auto directory = std::filesystem::path(next->path).parent_path();
        auto data = read_file(directory / "reference.mesh");
        size_t offset = 4;
        if (data.substr(0, 4) != "CGM1")
            throw std::runtime_error("Invalid reference mesh signature");
        auto count = integer(data, offset);
        next->reference = model(vertices(data, offset, count));
        if (offset != data.size())
            throw std::runtime_error("Trailing reference mesh data");
        data = read_file(directory / "reference.cells");
        offset = 4;
        if (data.substr(0, 4) != "CGC1" || integer(data, offset) != count / 3)
            throw std::runtime_error("Invalid reference cell ownership");
        for (int i = 0; i < count / 3; i++) {
            std::array<int, 3> cell;
            for (int k = 0; k < 3; k++)
                cell[k] = integer(data, offset);
            if (cell[0] < 0 || cell[0] >= next->size[0] ||
                cell[1] < -next->size[2] || cell[1] >= 0 || cell[2] < 0 ||
                cell[2] >= next->size[1])
                throw std::runtime_error(
                        "Reference owner exceeds context bounds");
            next->reference_cells.push_back(cell);
        }
        if (offset != data.size())
            throw std::runtime_error("Trailing reference cell data");
        data = read_file(directory / "palette.mesh");
        offset = 4;
        if (data.substr(0, 4) != "CGP1")
            throw std::runtime_error("Invalid palette mesh signature");
        while (offset < data.size()) {
            auto id = integer(data, offset);
            auto v = integer(data, offset);
            count = integer(data, offset);
            if (id < 1 || id > 255 || v < 0 || v > 3 ||
                !next->templates
                         .emplace(id * 4 + v, vertices(data, offset, count))
                         .second)
                throw std::runtime_error(
                        "Invalid or duplicate native block template");
        }
        int bpp = 4;
        auto atlas_path = (directory / "atlas.png").string();
        auto *pixels = img_read(
                atlas_path.c_str(), &next->atlas_w, &next->atlas_h, &bpp);
        if (!pixels || bpp != 4 || next->atlas_w != 432 ||
            next->atlas_h != 432)
        {
            free(pixels);
            throw std::runtime_error("Unexpected native atlas dimensions");
        }
        next->pixels.assign(pixels, pixels + 432 * 432 * 4);
        next->atlas = texture_new_from_buf(pixels, 432, 432, 4, TF_NEAREST);
        free(pixels);
        if (state &&
            (state->origin != next->origin || state->size != next->size ||
             state->manifest != next->manifest))
            throw std::runtime_error(
                    "Start a new document before changing its native context");
        if (state) {
            next->source = state->source;
            next->managed = state->managed;
            next->new_objects_solid = state->new_objects_solid;
        }
        if (!pending.empty()) {
            auto saved = saved_json(pending);
            if (next->manifest != string((*saved)["manifest"]))
                throw std::runtime_error(
                        "Choose the original saved native context");
            next->source = string((*saved)["source"]);
            next->managed = serialize(&(*saved)["managed"]);
            next->new_objects_solid = new_objects_solid(*saved);
        }
        saved_data(*next);
        update_metadata_key(*next);
        state = std::move(next);
        selected = variant = 0;
        choose_block();
        pending.clear();
        int aabb[2][3] = { { 0, -state->size[2], 0 },
                           { state->size[0], 0, state->size[1] } };
        bbox_from_aabb(goxel.image->box, aabb);
        goxel.hide_box = true;
        // Start brushes on real surfaces rather than the invisible bounding
        // box of the crop
        goxel.snap_mask = SNAP_VOLUME;
        if (frame)
            camera_fit_box(goxel.image->active_camera, goxel.image->box);
        snprintf(context_input, sizeof(context_input), "%s", path);
        if (goxel.image->active_layer)
            snprintf(goxel.image->active_layer->name,
                     sizeof(goxel.image->active_layer->name),
                     "Authored Crystal Edit voxels");
        status = "Native context loaded. Select a block below, then paint "
                 "against the world.";
        return true;
    }
    catch (const std::exception &error) {
        status = error.what();
        fprintf(stderr, "crystal-goxel: %s\n", error.what());
        return false;
    }
}

extern "C" void crystal_render(renderer_t *rend, bool preview)
{
    if (!state) return;
    try {
        uint32_t key = image_get_key(goxel.image);
        // Hover previews can hide an erased cell; picking must still see
        // committed authored content
        if (key != state->committed_key || !state->picking) {
            rebuild(false);
            state->picking = std::move(state->authored);
            state->picking_cells = std::move(state->authored_cells);
            state->committed_key = key;
            state->key = 0;
        }
        if (preview && goxel.tool_volume) {
            auto k = volume_get_key(goxel.tool_volume);
            key = XXH32(&k, sizeof(k), key);
        }
        if (key != state->key || !state->authored) {
            rebuild(preview);
            state->key = key;
        }
        if (state->show)
            render_mesh(rend, state->reference.get(), state->atlas,
                        EFFECT_NO_SHADING | EFFECT_ALPHA_CUTOUT);
        render_mesh(rend, state->authored.get(), state->atlas,
                    EFFECT_NO_SHADING | EFFECT_ALPHA_CUTOUT);
    }
    catch (const std::exception &error) {
        status = error.what();
    }
}

extern "C" bool crystal_pick(const camera_t *camera,
                             const float view[4],
                             const float pos[2],
                             float out[3],
                             float normal[3])
{
    if (!state) return false;
    float o[3], d[3];
    camera_get_ray(camera, pos, view, o, d);
    return pick_ray(o, d, out, normal);
}

extern "C" void crystal_panel(void)
{
    gui_text_wrapped("Build Crystal Edit voxels against the native world.");
    gui_input_text("Context", context_input, sizeof(context_input));
    if (gui_button("Open native context", 0, 0)) {
        const char *filters[] = { "*.json", nullptr };
        auto *path = sys_open_file_dialog(
                "Open native context", nullptr, filters, "Context JSON");
        if (path) crystal_load(path, true);
    }
    if (gui_button("Load path", 0, 0) && context_input[0])
        crystal_load(context_input, true);
    if (state) {
        gui_checkbox("Show native world", &state->show,
                     "Reference geometry is never part of authored exports");
        if (gui_button("Frame region", 0, 0))
            camera_fit_box(goxel.image->active_camera, goxel.image->box);
        gui_text("World origin: %d, %d, %d", state->origin[0],
                 state->origin[1], state->origin[2]);
        if (gui_combo_begin("Block", state->blocks[selected].name.c_str())) {
            for (int i = 0; i < int(state->blocks.size()); i++) {
                auto label = state->blocks[i].name + " [" +
                             std::to_string(state->blocks[i].id) + "]";
                if (gui_combo_item(label.c_str(), selected == i)) {
                    selected = i;
                    variant = 0;
                    choose_block();
                }
            }
            gui_combo_end();
        }
        const char *variants[] = { "0", "1", "2", "3" };
        if (gui_combo("Variant", &variant, variants,
                      state->blocks[selected].max_variant + 1))
            choose_block();
        if (gui_button("Use this block", 0, 0)) choose_block();
        gui_checkbox("Solid new objects", &state->new_objects_solid,
                     "Fixed in place, with full-cell collision when solid. "
                     "Uncheck for non-solid decoration. Imported objects "
                     "keep their existing physics settings");
        gui_text_wrapped("Use Goxel's brush, shape, selection and move tools. "
                         "Keep brush edges hard. The ordinary color palette "
                         "does not carry game block IDs.");
        try {
            const char *filters[] = { "*.json", nullptr };
            if (gui_button("Import Crystal Edit project", 0, 0)) {
                auto *path = sys_open_file_dialog(
                        "Import source project", nullptr, filters,
                        "Crystal Edit JSON");
                if (path) import_project(path);
            }
            if (gui_button("Export Crystal Edit project", 0, 0))
                export_dialog();
        }
        catch (const std::exception &error) {
            status = error.what();
        }
        gui_text_wrapped("Export includes all authored layers, including "
                         "hidden layers. Imported static voxel objects are "
                         "editable; other entities and unknown fields are "
                         "preserved. Native terrain is reference only.");
        if (state->invalid)
            gui_text("Invalid block identities: %zu", state->invalid);
    }
    if (!status.empty()) gui_text_wrapped("%s", status.c_str());
}

extern "C" bool crystal_save_state(char **data, size_t *size)
{
    *data = nullptr;
    *size = 0;
    try {
        auto payload = state ? saved_data(*state) : pending;
        if (payload.empty()) return true;
        *data = strdup(payload.c_str());
        if (!*data) throw std::bad_alloc();
        *size = payload.size();
        return true;
    }
    catch (const std::exception &error) {
        status = error.what();
        fprintf(stderr, "crystal-goxel save: %s\n", error.what());
        return false;
    }
}

extern "C" uint32_t crystal_project_key(void)
{
    if (!state) return pending.empty() ? 0 : pending_key;
    return XXH32(&state->new_objects_solid, sizeof(state->new_objects_solid),
                 state->metadata_key);
}

extern "C" void crystal_restore_state(const char *data, size_t size)
{
    try {
        if (size > MAX_STATE)
            throw std::runtime_error("Bridge project state exceeds its limit");
        // Keep unavailable-context state intact so opening and resaving never
        // drops the original source
        pending.assign(data, size);
        pending_key = XXH32(pending.data(), pending.size(), 0);
        auto root = saved_json(pending);
        std::string path = string((*root)["context"]);
        std::string expected = string((*root)["manifest"]);
        if (read_file(path, MAX_STATE) != expected)
            throw std::runtime_error("Saved context fingerprint changed");
        if (!crystal_load(path.c_str(), false))
            throw std::runtime_error(status);
        state->source = string((*root)["source"]);
        state->managed = serialize(&(*root)["managed"]);
        state->new_objects_solid = new_objects_solid(*root);
        update_metadata_key(*state);
    }
    catch (const std::exception &error) {
        status = error.what();
        fprintf(stderr, "crystal-goxel restore: %s\n", error.what());
    }
}

extern "C" int crystal_smoke(const char *output)
{
    try {
        if (!state)
            throw std::runtime_error("Smoke check requires --crystal-context");
        if (!goxel.graphics_initialized) goxel_create_graphics();
        goxel.rend.scale = 1;
        std::vector<uint8_t> initial(1024 * 768 * 4);
        goxel_render_to_buf(initial.data(), 1024, 768, 4);
        state->show = false;
        std::vector<uint8_t> empty(1024 * 768 * 4);
        goxel_render_to_buf(empty.data(), 1024, 768, 4);
        state->show = true;
        if (initial == empty)
            throw std::runtime_error(
                    "Native reference produced no GPU-visible pixels");
        float origin[3] = { state->size[0] * .5f + .5f,
                            -state->size[2] * .5f - .5f,
                            float(state->size[1] + 10) };
        float down[3] = { 0, 0, -1 }, hit[3], normal[3];
        if (!pick_ray(origin, down, hit, normal))
            throw std::runtime_error("Native reference picking failed");
        if (cells_json() != "[]")
            throw std::runtime_error(
                    "Brush smoke requires an empty authored document");
        image_history_push(goxel.image);
        inputs_t input = {};
        input.window_size[0] = 1024;
        input.window_size[1] = 768;
        input.scale = 1;
        input.touches[0].pos[0] = input.touches[0].pos[1] = -1;
        auto frame = [&]() {
            goxel_iter(&input);
            goxel_render(&input);
        };
        frame();
        float screen[3];
        camera_project(goxel.image->active_camera, hit, goxel.gui.viewport,
                       screen);
        input.touches[0].pos[0] = screen[0];
        input.touches[0].pos[1] = input.window_size[1] - screen[1];
        auto click = [&]() {
            frame();
            frame();
            input.touches[0].down[0] = true;
            frame();
            frame();
            input.touches[0].down[0] = false;
            frame();
            frame();
            input.touches[0].pos[0] = -1;
            frame();
            input.touches[0].pos[0] = screen[0];
        };
        click();
        auto painted = cells_json();
        if (painted == "[]")
            throw std::runtime_error(
                    "Synthetic mouse brush stroke did not commit a cell");
        image_undo(goxel.image);
        if (cells_json() != "[]")
            throw std::runtime_error(
                    "Brush undo did not restore empty authored content");
        image_redo(goxel.image);
        if (cells_json() != painted)
            throw std::runtime_error("Brush redo changed native identity");
        goxel.painter.mode = MODE_SUB;
        // Erase the painted object through the same screen-input path used by
        // the visible app
        frame();
        click();
        if (cells_json() != "[]") {
            fprintf(stderr,
                    "crystal-goxel brush diagnostic: painted=%s "
                    "afterErase=%s\n",
                    painted.c_str(), cells_json().c_str());
            throw std::runtime_error(
                    "Synthetic mouse erase did not remove the authored cell");
        }
        image_undo(goxel.image);
        goxel.painter.mode = MODE_OVER;
        if (cells_json() != painted)
            throw std::runtime_error("Erase undo changed native identity");
        std::vector<uint8_t> pixels(1024 * 768 * 4);
        goxel_render_to_buf(pixels.data(), 1024, 768, 4);
        img_write(pixels.data(), 1024, 768, 4, output);
        auto project = std::string(output) + ".gox";
        save_to_file(goxel.image, project.c_str());
        {
            struct RestoreDialogs {
                sys_callbacks_t previous = sys_callbacks;
                ~RestoreDialogs()
                {
                    sys_callbacks = previous;
                }
            } restore;
            struct DialogProbe {
                std::string path;
                bool accept = false, valid = false;
                int calls = 0;
            } probe{ std::string(output) + ".json" };
            // Exercise the button's dialog request while selecting files
            // without modal UI in the hidden-window check
            sys_callbacks.user = &probe;
            sys_callbacks.open_dialog =
                    [](void *user, char *buf, size_t size, int flags,
                       const char *, const char *default_name, int count,
                       const char *const *filters, const char *) {
                        auto &probe = *static_cast<DialogProbe *>(user);
                        probe.calls++;
                        probe.valid = flags == 1 && default_name &&
                                      std::filesystem::path(default_name)
                                                      .extension() ==
                                              ".json" &&
                                      count == 1 && filters && filters[0] &&
                                      strcmp(filters[0], "*.json") == 0;
                        if (!probe.accept || !probe.valid) return false;
                        if (probe.path.size() >= size) {
                            probe.valid = false;
                            return false;
                        }
                        memcpy(buf, probe.path.c_str(), probe.path.size() + 1);
                        return true;
                    };
            auto before_export = cells_json();
            export_dialog();
            if (!probe.valid || probe.calls != 1 ||
                std::filesystem::exists(probe.path) ||
                cells_json() != before_export)
                throw std::runtime_error(
                        "Cancelled export dialog changed the project or "
                        "received no JSON filename");
            probe.accept = true;
            export_dialog();
            if (!probe.valid || probe.calls != 2 ||
                !std::filesystem::exists(probe.path) ||
                cells_json() != before_export)
                throw std::runtime_error(
                        "Accepted export dialog did not create the project");
        }
        auto before = cells_json();
        if (load_from_file(project.c_str(), true) != 0 ||
            cells_json() != before)
            throw std::runtime_error(
                    "Native authoring project round trip failed");

        // Exercise the UI import path and its opaque source text through
        // actual GOX persistence
        auto original = read_file(std::string(output) + ".json");
        auto source = original;
        source.insert(source.find('{') + 1,
                      "\"BridgeSmokeUnknown\":{\"large\":9007199254740993,"
                      "\"text\":\"quoted \\\"x\\\"\"},");
        auto source_path = std::string(output) + ".import-source.json";
        std::ofstream(source_path) << source;
        for (auto *layer = goxel.image->layers; layer; layer = layer->next)
            volume_clear(layer->volume);
        import_project(source_path.c_str());
        if (cells_json() != before || state->source != source)
            throw std::runtime_error("UI import changed native identities or "
                                     "original source text");
        auto solid_key = image_get_key(goxel.image);
        state->new_objects_solid = false;
        if (image_get_key(goxel.image) == solid_key)
            throw std::runtime_error(
                    "Export setting did not mark the project changed");
        auto saved = saved_data(*state);
        auto context_path = state->path;
        if (!crystal_load(context_path.c_str(), false) ||
            saved_data(*state) != saved)
            throw std::runtime_error(
                    "Reloading context lost imported source metadata");
        project = std::string(output) + ".imported.gox";
        save_to_file(goxel.image, project.c_str());
        if (load_from_file(project.c_str(), true) != 0 || !state ||
            cells_json() != before || saved_data(*state) != saved ||
            state->new_objects_solid)
            throw std::runtime_error(
                    "Imported source metadata did not survive save/reopen");
        export_project((std::string(output) + ".imported.json").c_str());
        auto exported = read_file(std::string(output) + ".imported.json");
        if (exported.find("9007199254740993") == std::string::npos)
            throw std::runtime_error(
                    "Export lost an unknown large numeric field");

        auto metadata = saved_json(saved);
        auto unavailable =
                "{\"format\":1,\"context\":" +
                quote(source_path + ".missing") +
                ",\"manifest\":" + quote(string((*metadata)["manifest"])) +
                ",\"source\":" + quote(string((*metadata)["source"])) +
                ",\"managed\":" + serialize(&(*metadata)["managed"]) +
                ",\"newObjectsSolid\":false}";
        crystal_reset();
        crystal_restore_state(unavailable.data(), unavailable.size());
        char *retained = nullptr;
        size_t retained_size;
        if (state || !crystal_save_state(&retained, &retained_size))
            throw std::runtime_error("Missing context recovery failed");
        std::string retained_text(retained, retained_size);
        free(retained);
        if (retained_text != unavailable)
            throw std::runtime_error(
                    "Missing context state was not retained exactly");
        if (!crystal_load(context_path.c_str(), false) ||
            state->source != source || cells_json() != before ||
            state->new_objects_solid)
            throw std::runtime_error("Selecting the original context did not "
                                     "restore imported state");
        auto legacy = saved;
        const std::string option = ",\"newObjectsSolid\":false";
        legacy.erase(legacy.find(option), option.size());
        crystal_reset();
        crystal_restore_state(legacy.data(), legacy.size());
        if (!state || !state->new_objects_solid || state->source != source)
            throw std::runtime_error(
                    "Legacy project did not restore solid defaults");
        auto valid_source = state->source;
        state->source.assign(MAX_STATE + 1, 'x');
        auto invalid_path = std::string(output) + ".oversized.gox";
        save_to_file(goxel.image, invalid_path.c_str());
        state->source = std::move(valid_source);
        if (std::filesystem::exists(invalid_path))
            throw std::runtime_error(
                    "Oversized state save touched its destination");
        printf("crystal-goxel smoke: nativeVertices=%d mouseBrush=ok "
               "mouseErase=ok undoRedo=ok picking=ok exportDialog=ok "
               "persistence=ok "
               "importExport=ok missingContext=ok exportSettings=ok "
               "saveLimit=ok\n",
               state->reference->nb_vertices);
        return 0;
    }
    catch (const std::exception &error) {
        fprintf(stderr, "crystal-goxel smoke: %s\n", error.what());
        return 1;
    }
}
