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
#include <chrono>
#include <cctype>
#include <cmath>
#include "crystal_host.h"
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint8_t TOKEN_SIGNATURE = 0xc7;
constexpr size_t MAX_ASSET = 256 * 1024 * 1024;
constexpr size_t MAX_STATE = CRYSTAL_MAX_STATE;
constexpr size_t MAX_PREVIEW_VERTICES = 4'000'000;
constexpr int NATIVE_TILE_EDGE = 16;
constexpr int MAX_VIEW_TILES = 125;
constexpr int VIEW_TILE_RADIUS = 2;
constexpr int PREFETCH_TILE_RADIUS = VIEW_TILE_RADIUS + 1;
constexpr int MAX_PREFETCH_TILES = 343;
constexpr size_t MAX_CACHED_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_CACHED_TILES = 1024;
constexpr int CAMERA_EDGE_MARGIN = NATIVE_TILE_EDGE;
constexpr float CAMERA_POSITION_EPSILON = .001f;
constexpr int WORLD_EXTENT = 10000;
constexpr int WORLD_HEIGHT = 256;
constexpr unsigned MAX_BOOKMARKS = 64;
constexpr unsigned MAX_NATIVE_LOCATIONS = 256;
constexpr size_t MAX_LOCATION_BYTES = 256 * 1024;
constexpr size_t MAX_LOCATION_LABEL_BYTES = 1024;
constexpr char HOME_POINT_KIND[] = "homePoint";
constexpr char TELEPORT_POINT_KIND[] = "teleportPoint";
constexpr char DEFAULT_EXPORT_NAME[] = "crystal-project.json";
static_assert((VIEW_TILE_RADIUS * 2 + 1) * (VIEW_TILE_RADIUS * 2 + 1) *
              (VIEW_TILE_RADIUS * 2 + 1) <= MAX_VIEW_TILES,
              "Navigation must fit the tile preparation limit");
using Json = std::unique_ptr<json_value, decltype(&json_value_free)>;
using Model = std::unique_ptr<model3d_t, decltype(&model3d_delete)>;
struct Block {
    int id, max_variant;
    std::string name;
};
struct NativeLocation {
    int id;
    std::string kind, name, area;
    std::array<int, 3> world;
    bool name_resolved;
};
struct FileStamp {
    uintmax_t size;
    std::filesystem::file_time_type time;
    bool operator==(const FileStamp &other) const
    {
        return size == other.size && time == other.time;
    }
};
struct TerrainTile {
    std::filesystem::path directory;
    std::array<int, 3> key;
    std::array<FileStamp, 4> stamps;
    std::vector<model_vertex_t> mesh;
    std::vector<std::array<int, 3>> owners;
    uint64_t used = 0;
    size_t bytes() const
    {
        return sizeof(*this) + directory.native().size() +
               mesh.capacity() * sizeof(model_vertex_t) +
               owners.capacity() * sizeof(std::array<int, 3>);
    }
};
using Tile = std::shared_ptr<TerrainTile>;
using TileCache = std::map<std::array<int, 3>, Tile>;
struct PreparedTerrain {
    std::array<int, 3> min, max;
    TileCache tiles;
    bool from_memory = false;
};
struct TerrainJob {
    std::string path;
    std::array<int, 3> center;
    uint64_t revision, document;
    bool prefetch;
    std::chrono::steady_clock::time_point started;
    std::future<PreparedTerrain> result;
};
uint64_t next_document = 0;
struct State {
    uint64_t document = ++next_document;
    std::string path, manifest, source, managed = "[]";
    std::string identities = "[]", bookmarks = "[]";
    std::array<int, 3> origin{}, size{};
    std::array<int, 3> center{ 1, 99, 1 }, view_min{}, view_max{};
    std::set<std::array<int, 3>> tiles;
    TileCache cache;
    size_t cache_bytes = 0;
    uint64_t cache_clock = 0, view_revision = 0;
    size_t memory_views = 0;
    bool prefetch_attempted = false;
    std::string prefetch_error;
    bool tiled = false;
    bool follow_view = true;
    int height_step = NATIVE_TILE_EDGE;
    bool height_up_pressed = false, height_down_pressed = false;
    std::vector<NativeLocation> locations;
    std::string locations_error;
    char location_search[128] = {};
    int location_selected = 0;
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
// Workers only prepare private cache files and CPU data; GL publication stays
// on the editor thread, and reset/context changes invalidate their results
std::unique_ptr<TerrainJob> terrain_job;
std::string helper, status, pending;
uint32_t pending_key = 0;
char context_input[4096] = {};
char installation_input[4096] = {};
char cache_input[4096] = {};
char bookmark_input[128] = {};
int location_input[3] = { 1, 99, 1 };
std::string failed_tile_request;
int selected = 0, variant = 0;

std::array<int, 3> triple(const json_value &value);

void check_location(const std::array<int, 3> &point)
{
    if (std::abs(int64_t(point[0])) > WORLD_EXTENT ||
        std::abs(int64_t(point[2])) > WORLD_EXTENT ||
        point[1] < 0 || point[1] >= WORLD_HEIGHT)
        throw std::runtime_error("Location exceeds supported world bounds");
}

std::string read_file(const std::filesystem::path &path,
                      size_t limit = MAX_ASSET)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.u8string());
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
         { &saved.path, &saved.manifest, &saved.source, &saved.managed,
           &saved.identities, &saved.bookmarks })
        saved.metadata_key = XXH32(
                value->data(), value->size(), saved.metadata_key);
    saved.metadata_key = XXH32(saved.center.data(), sizeof(saved.center),
                              saved.metadata_key);
}

std::string saved_data(const State &saved)
{
    auto data = "{\"format\":1,\"context\":" + quote(saved.path) +
                ",\"manifest\":" + quote(saved.manifest) +
                ",\"source\":" + quote(saved.source) +
                ",\"managed\":" + saved.managed + ",\"newObjectsSolid\":" +
                (saved.new_objects_solid ? "true" : "false");
    if (saved.tiled)
        data += ",\"identities\":" + saved.identities +
                ",\"bookmarks\":" + saved.bookmarks +
                ",\"location\":[" + std::to_string(saved.center[0]) + "," +
                std::to_string(saved.center[1]) + "," +
                std::to_string(saved.center[2]) + "]";
    data += '}';
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
    for (const char *key : { "identities", "bookmarks", "location" })
        if ((*root)[key].type != json_none &&
            (*root)[key].type != json_array)
            throw std::runtime_error("Invalid saved world document metadata");
    if ((*root)["location"].type != json_none)
        check_location(triple((*root)["location"]));
    const auto &bookmarks = (*root)["bookmarks"];
    if (bookmarks.type != json_none) {
        if (bookmarks.u.array.length > MAX_BOOKMARKS)
            throw std::runtime_error("Location bookmark limit reached");
        std::set<std::string> names;
        for (unsigned i = 0; i < bookmarks.u.array.length; i++) {
            auto name = string(bookmarks[i]["name"]);
            if (name.empty() || name.size() >= sizeof(bookmark_input) ||
                !names.insert(name).second)
                throw std::runtime_error("Invalid saved location name");
            check_location(triple(bookmarks[i]["world"]));
        }
    }
    return root;
}

Json helper_report(const std::string &output)
{
    // The helper writes a JSON result before its diagnostic line
    return parse(output.substr(0, output.find('\n')));
}

using Temporary = crystal_host::Temporary;

std::string run_helper(const std::vector<std::string> &arguments,
                       const std::string &executable = helper)
{
    return crystal_host::run(executable, arguments);
}

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

std::string triple(const std::array<int, 3> &point)
{
    return std::to_string(point[0]) + "," + std::to_string(point[1]) + "," +
           std::to_string(point[2]);
}

std::array<int, 3> triple(const json_value &value)
{
    if (value.type != json_array || value.u.array.length != 3)
        throw std::runtime_error("Expected three world coordinates");
    return { number(value[0]), number(value[1]), number(value[2]) };
}

int tile_index(int coordinate)
{
    // Integer division truncates toward zero; negative world cells need floor
    return int(std::floor(double(coordinate) / NATIVE_TILE_EDGE));
}

void view_box(const State &saved, float box[4][4])
{
    int bounds[2][3];
    if (saved.tiled) {
        int world[2][3] = {
            { saved.view_min[0], -saved.view_max[2], saved.view_min[1] },
            { saved.view_max[0], -saved.view_min[2], saved.view_max[1] }
        };
        memcpy(bounds, world, sizeof(bounds));
    } else {
        int local[2][3] = {
            { 0, -saved.size[2], 0 },
            { saved.size[0], 0, saved.size[1] }
        };
        memcpy(bounds, local, sizeof(bounds));
    }
    bbox_from_aabb(box, bounds);
}

void reference_geometry(const std::filesystem::path &directory,
                        const std::array<int, 3> &origin,
                        const std::array<int, 3> &size, bool global,
                        std::vector<model_vertex_t> &mesh,
                        std::vector<std::array<int, 3>> &owners)
{
    auto data = read_file(directory / "reference.mesh");
    size_t offset = 4;
    if (data.substr(0, 4) != "CGM1")
        throw std::runtime_error("Invalid reference mesh signature");
    auto count = integer(data, offset);
    auto part = vertices(data, offset, count);
    if (offset != data.size() || mesh.size() + part.size() >
                                MAX_PREVIEW_VERTICES)
        throw std::runtime_error("Reference mesh exceeds its view limit");
    data = read_file(directory / "reference.cells");
    offset = 4;
    if (data.substr(0, 4) != "CGC1" || integer(data, offset) != count / 3)
        throw std::runtime_error("Invalid reference cell ownership");
    for (int i = 0; i < count / 3; i++) {
        std::array<int, 3> cell;
        for (int k = 0; k < 3; k++) cell[k] = integer(data, offset);
        if (cell[0] < 0 || cell[0] >= size[0] || cell[1] < -size[2] ||
            cell[1] >= 0 || cell[2] < 0 || cell[2] >= size[1])
            throw std::runtime_error("Reference owner exceeds tile bounds");
        if (global) {
            cell[0] += origin[0];
            cell[1] -= origin[2];
            cell[2] += origin[1];
        }
        owners.push_back(cell);
    }
    if (offset != data.size())
        throw std::runtime_error("Trailing reference ownership data");
    for (auto &vertex : part) {
        if (global) {
            vertex.pos[0] += origin[0];
            vertex.pos[1] -= origin[2];
            vertex.pos[2] += origin[1];
        }
        mesh.push_back(vertex);
    }
}

std::array<FileStamp, 4> tile_stamps(const std::filesystem::path &path)
{
    std::array<FileStamp, 4> result;
    const char *names[] = { "tile.json", "reference.mesh", "reference.cells",
                            "biomes.bin" };
    for (int i = 0; i < 4; i++) {
        auto file = path / names[i];
        result[i] = { std::filesystem::file_size(file),
                      std::filesystem::last_write_time(file) };
    }
    return result;
}

bool tile_unchanged(const Tile &tile)
{
    try { return tile->stamps == tile_stamps(tile->directory); }
    catch (const std::exception &) { return false; }
}

TileCache reusable_tiles(State &saved)
{
    TileCache result;
    for (auto it = saved.cache.begin(); it != saved.cache.end();) {
        // File metadata only invalidates cached data; the helper remains the
        // fingerprint authority whenever a tile is first read or changed
        if (!tile_unchanged(it->second)) {
            saved.cache_bytes -= it->second->bytes();
            it = saved.cache.erase(it);
        } else {
            result.insert(*it);
            ++it;
        }
    }
    return result;
}

PreparedTerrain read_prepared(const std::string &path,
                              const std::array<int, 3> &min,
                              const std::array<int, 3> &max,
                              const TileCache &cache, bool prefetch)
{
    auto view = parse(read_file(path, MAX_STATE));
    const auto &tiles = (*view)["tiles"];
    auto limit = prefetch ? MAX_PREFETCH_TILES : MAX_VIEW_TILES;
    if (tiles.type != json_array || tiles.u.array.length == 0 ||
        tiles.u.array.length > unsigned(limit))
        throw std::runtime_error("Invalid prepared tile view");
    PreparedTerrain result{ min, max, {} };
    size_t bytes = 0;
    std::set<std::array<int, 3>> ready;
    for (unsigned i = 0; i < tiles.u.array.length; i++) {
        const auto &entry = tiles[i];
        auto origin = triple(entry["origin"]);
        std::array<int, 3> key;
        for (int k = 0; k < 3; k++) {
            if (origin[k] % NATIVE_TILE_EDGE ||
                tile_index(origin[k]) < tile_index(min[k]) ||
                tile_index(origin[k]) > tile_index(max[k] - 1))
                throw std::runtime_error("Prepared tile is outside the view");
            key[k] = tile_index(origin[k]);
        }
        if (!ready.insert(key).second)
            throw std::runtime_error("Duplicate prepared terrain tile");
        // Speculative CPU data is bounded even when native geometry is dense
        if (prefetch && bytes >= MAX_CACHED_BYTES) continue;
        auto found = cache.find(key);
        Tile tile;
        if (found != cache.end() && tile_unchanged(found->second))
            tile = found->second;
        else {
            tile = std::make_shared<TerrainTile>();
            tile->key = key;
            tile->directory = string(entry["path"]);
            tile->stamps = tile_stamps(tile->directory);
            reference_geometry(tile->directory, origin,
                               { NATIVE_TILE_EDGE, NATIVE_TILE_EDGE,
                                 NATIVE_TILE_EDGE }, true,
                               tile->mesh, tile->owners);
            if (!tile_unchanged(tile))
                throw std::runtime_error("Terrain tile changed while reading");
        }
        if (prefetch && tile->bytes() > MAX_CACHED_BYTES - bytes) continue;
        bytes += tile->bytes();
        result.tiles.emplace(key, std::move(tile));
    }
    size_t expected = 1;
    for (int k = 0; k < 3; k++)
        expected *= size_t(tile_index(max[k] - 1) - tile_index(min[k]) + 1);
    if (ready.size() != expected)
        throw std::runtime_error("Prepared view is missing a terrain tile");
    return result;
}

PreparedTerrain prepare_terrain(const std::string &context,
                                const std::string &executable,
                                const std::array<int, 3> &min,
                                const std::array<int, 3> &max,
                                const TileCache &cache, bool prefetch)
{
    PreparedTerrain ready{ min, max, {} };
    bool complete = true;
    for (int x = tile_index(min[0]); x <= tile_index(max[0] - 1); x++)
        for (int y = tile_index(min[1]); y <= tile_index(max[1] - 1); y++)
            for (int z = tile_index(min[2]); z <= tile_index(max[2] - 1); z++) {
                auto found = cache.find({ x, y, z });
                if (found == cache.end() || !tile_unchanged(found->second))
                    complete = false;
                else ready.tiles.insert(*found);
            }
    if (complete) {
        ready.from_memory = true;
        return ready;
    }
    Temporary temp;
    auto path = temp.directory / "view.json";
    run_helper({ prefetch ? "prefetch" : "tiles", "--context", context,
                 "--min", triple(min), "--max", triple(max),
                 "--output", path.u8string() }, executable);
    return read_prepared(path.u8string(), min, max, cache, prefetch);
}

void trim_terrain_cache(State &saved, size_t byte_limit, size_t tile_limit)
{
    while (saved.cache_bytes > byte_limit || saved.cache.size() > tile_limit) {
        auto oldest = std::min_element(saved.cache.begin(), saved.cache.end(),
                [](const auto &a, const auto &b) {
                    return a.second->used < b.second->used;
                });
        saved.cache_bytes -= oldest->second->bytes();
        saved.cache.erase(oldest);
    }
}

void cache_terrain(State &saved, const PreparedTerrain &prepared)
{
    for (const auto &entry : prepared.tiles) {
        auto found = saved.cache.find(entry.first);
        if (found != saved.cache.end())
            saved.cache_bytes -= found->second->bytes();
        entry.second->used = ++saved.cache_clock;
        saved.cache[entry.first] = entry.second;
        saved.cache_bytes += entry.second->bytes();
    }
    trim_terrain_cache(saved, MAX_CACHED_BYTES, MAX_CACHED_TILES);
}

void publish_terrain(State &saved, const PreparedTerrain &prepared)
{
    std::vector<model_vertex_t> mesh;
    std::vector<std::array<int, 3>> owners;
    std::set<std::array<int, 3>> ready;
    for (const auto &entry : prepared.tiles) {
        if (!tile_unchanged(entry.second))
            throw std::runtime_error("Terrain tile changed before publication");
        if (mesh.size() + entry.second->mesh.size() > MAX_PREVIEW_VERTICES)
            throw std::runtime_error("Reference mesh exceeds its view limit");
        mesh.insert(mesh.end(), entry.second->mesh.begin(),
                    entry.second->mesh.end());
        owners.insert(owners.end(), entry.second->owners.begin(),
                      entry.second->owners.end());
        ready.insert(entry.first);
    }
    auto reference = model(mesh);
    cache_terrain(saved, prepared);
    // Geometry, picking ownership and loaded bounds are published together
    saved.reference = std::move(reference);
    saved.reference_cells = std::move(owners);
    saved.tiles = std::move(ready);
    for (int k = 0; k < 3; k++) {
        saved.view_min[k] = tile_index(prepared.min[k]) * NATIVE_TILE_EDGE;
        saved.view_max[k] = (tile_index(prepared.max[k] - 1) + 1) *
                           NATIVE_TILE_EDGE;
        saved.size[k] = saved.view_max[k] - saved.view_min[k];
    }
    saved.view_revision++;
    if (prepared.from_memory) saved.memory_views++;
    saved.prefetch_attempted = false;
    saved.prefetch_error.clear();
    saved.authored.reset();
    saved.picking.reset();
}

void prepare_view(State &saved, const std::array<int, 3> &min,
                  const std::array<int, 3> &max)
{
    auto cache = reusable_tiles(saved);
    auto prepared = prepare_terrain(saved.path, helper, min, max, cache, false);
    if (prepared.tiles.size() > MAX_VIEW_TILES)
        throw std::runtime_error("Prepared view exceeds its tile limit");
    publish_terrain(saved, prepared);
}

void location_bounds(const std::array<int, 3> &center, int radius,
                     std::array<int, 3> &min, std::array<int, 3> &max)
{
    check_location(center);
    for (int k = 0; k < 3; k++) {
        min[k] = (tile_index(center[k]) - radius) * NATIVE_TILE_EDGE;
        max[k] = (tile_index(center[k]) + radius + 1) * NATIVE_TILE_EDGE;
        auto lower = k == 1 ? 0 : -WORLD_EXTENT;
        auto upper = k == 1 ? WORLD_HEIGHT : WORLD_EXTENT + 1;
        min[k] = std::max(min[k], lower);
        max[k] = std::min(max[k], upper);
    }
}

void location_view(State &saved, const std::array<int, 3> &center)
{
    std::array<int, 3> min, max;
    location_bounds(center, VIEW_TILE_RADIUS, min, max);
    prepare_view(saved, min, max);
    saved.center = center;
}

void restore_metadata(State &saved, const json_value &root)
{
    saved.source = string(root["source"]);
    saved.managed = serialize(&root["managed"]);
    saved.new_objects_solid = new_objects_solid(root);
    if (root["identities"].type != json_none)
        saved.identities = serialize(&root["identities"]);
    if (root["bookmarks"].type != json_none)
        saved.bookmarks = serialize(&root["bookmarks"]);
    if (root["location"].type != json_none)
        saved.center = triple(root["location"]);
}

void visit_location(const std::array<int, 3> &center, bool frame)
{
    location_view(*state, center);
    update_metadata_key(*state);
    std::copy(center.begin(), center.end(), location_input);
    if (frame) {
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = nullptr;
        float box[4][4];
        view_box(*state, box);
        camera_fit_box(goxel.image->active_camera, box);
    }
    failed_tile_request.clear();
    status = "Location loaded. Edits at other locations remain in this mod.";
}

void load_native_locations(State &saved)
{
    try {
        Temporary work;
        auto output = (work.directory / "locations.json").u8string();
        run_helper({ "locations", "--context", saved.path,
                     "--output", output });
        auto root = parse(read_file(output, MAX_LOCATION_BYTES));
        const auto &entries = (*root)["entries"];
        if (entries.type != json_array || !entries.u.array.length ||
            entries.u.array.length > MAX_NATIVE_LOCATIONS)
            throw std::runtime_error("Invalid native location catalog");
        std::vector<NativeLocation> locations;
        std::set<std::string> ids;
        for (unsigned i = 0; i < entries.u.array.length; i++) {
            const auto &entry = entries[i];
            auto point = triple(entry["world"]);
            check_location(point);
            auto id = number(entry["id"]);
            auto kind = string(entry["kind"]);
            auto name = string(entry["name"]), area = string(entry["area"]);
            if ((kind != HOME_POINT_KIND && kind != TELEPORT_POINT_KIND) ||
                id < 0 || !ids.insert(kind + ":" + std::to_string(id)).second ||
                name.empty() ||
                area.empty() || name.size() > MAX_LOCATION_LABEL_BYTES ||
                area.size() > MAX_LOCATION_LABEL_BYTES ||
                entry["nameResolved"].type != json_boolean)
                throw std::runtime_error("Invalid native location entry");
            locations.push_back({ id, kind, name, area, point,
                                 bool(entry["nameResolved"].u.boolean) });
        }
        std::sort(locations.begin(), locations.end(),
                  [](const NativeLocation &left, const NativeLocation &right) {
                      if (left.area != right.area)
                          return left.area < right.area;
                      if (left.name != right.name)
                          return left.name < right.name;
                      if (left.kind != right.kind)
                          return left.kind < right.kind;
                      return left.id < right.id;
                  });
        saved.locations = std::move(locations);
        auto distance = [&saved](const NativeLocation &location) {
            int64_t result = 0;
            for (int i = 0; i < 3; i++) {
                int64_t delta = location.world[i] - saved.center[i];
                result += delta * delta;
            }
            return result;
        };
        auto nearest = std::min_element(saved.locations.begin(),
                                       saved.locations.end(),
                [&distance](const NativeLocation &left,
                            const NativeLocation &right) {
                    return distance(left) < distance(right);
                });
        saved.location_selected = nearest - saved.locations.begin();
        saved.locations_error.clear();
        if ((*root)["warning"].type == json_string)
            saved.locations_error = string((*root)["warning"]);
    }
    catch (const std::exception &error) {
        // Optional navigation cannot prevent opening terrain or authored work
        saved.locations_error = error.what();
        fprintf(stderr, "crystal-goxel locations: %s\n", error.what());
    }
}

bool location_matches(const NativeLocation &location, const char *query)
{
    auto lower = [](std::string text) {
        for (auto &c : text) c = char(std::tolower(uint8_t(c)));
        return text;
    };
    return lower(location.area + " " + location.name).find(lower(query)) !=
           std::string::npos;
}

const char *native_location_type(const NativeLocation &location)
{
    return location.kind == TELEPORT_POINT_KIND ? "Landmark" : "Home point";
}

void native_locations_panel()
{
    if (gui_section_begin("Native locations", GUI_SECTION_COLLAPSABLE)) {
        gui_text("Search places");
        gui_input_text("##native-location-search", state->location_search,
                       sizeof(state->location_search));
        std::vector<int> matches;
        for (unsigned i = 0; i < state->locations.size(); i++)
            if (location_matches(state->locations[i], state->location_search))
                matches.push_back(i);
        if (!matches.empty()) {
            if (std::find(matches.begin(), matches.end(),
                          state->location_selected) == matches.end())
                state->location_selected = matches.front();
            auto &current = state->locations[state->location_selected];
            if (gui_combo_begin("##native-location", current.name.c_str())) {
                for (int i : matches) {
                    auto &location = state->locations[i];
                    auto label = location.name + " [" +
                                 native_location_type(location) + "] - " +
                                 location.area +
                                 "##native-location-" +
                                 location.kind + std::to_string(location.id);
                    if (gui_combo_item(label.c_str(),
                                       state->location_selected == i))
                        state->location_selected = i;
                }
                gui_combo_end();
            }
            auto &location = state->locations[state->location_selected];
            gui_text_wrapped("%s", location.area.c_str());
            gui_text_wrapped("%s", location.name.c_str());
            gui_text("%s", native_location_type(location));
            gui_text("World: %d, %d, %d", location.world[0],
                     location.world[1], location.world[2]);
            if (!location.name_resolved)
                gui_text_wrapped("This place's native name uses "
                                 "unresolved text variables.");
            if (gui_button("Go to selected place", 0, 0))
                visit_location(location.world, true);
        } else if (state->locations_error.empty()) {
            gui_text_wrapped("No places match your search.");
        }
        if (!state->locations_error.empty()) {
            gui_text_wrapped(state->locations.empty() ?
                             "Native locations unavailable: %s" :
                             "Some landmarks unavailable: %s",
                             state->locations_error.c_str());
            if (gui_button("Retry native locations", 0, 0))
                load_native_locations(*state);
        }
    }
    gui_section_end();
}

void add_bookmark(const std::string &name)
{
    if (name.empty() || name.size() >= sizeof(bookmark_input))
        throw std::runtime_error("Enter a short location name");
    auto root = parse("{\"items\":" + state->bookmarks + "}");
    const auto &items = (*root)["items"];
    if (items.u.array.length >= MAX_BOOKMARKS)
        throw std::runtime_error("Location bookmark limit reached");
    for (unsigned i = 0; i < items.u.array.length; i++)
        if (string(items[i]["name"]) == name)
            throw std::runtime_error("Choose a unique location name");
    auto next = state->bookmarks;
    next.pop_back();
    if (items.u.array.length) next += ',';
    next += "{\"name\":" + quote(name) + ",\"world\":[" +
            std::to_string(state->center[0]) + "," +
            std::to_string(state->center[1]) + "," +
            std::to_string(state->center[2]) + "]}]";
    auto normalized = parse("{\"items\":" + next + "}");
    state->bookmarks = serialize(&(*normalized)["items"]);
    update_metadata_key(*state);
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
                               path, "--output", snapshot.u8string() });
    auto result = helper_report(report);
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
    metadata.tiled = state->tiled;
    metadata.center = state->center;
    metadata.bookmarks = state->bookmarks;
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
    state->identities = "[]";
    update_metadata_key(*state);
    image_history_push(goxel.image);
    status = "Imported " + std::to_string(number((*result)["imported"])) +
             " editable objects. Preserved " +
             std::to_string(number((*result)["preserved"])) +
             " other entities in the source project.";
}

void export_project(const char *path)
{
    Temporary temp;
    auto snapshot = temp.directory / "snapshot.json";
    std::ofstream output(snapshot);
    output << "{\"format\":1,\"source\":" << quote(state->source)
           << ",\"managed\":" << state->managed << ",\"newObjectsSolid\":"
           << (state->new_objects_solid ? "true" : "false")
           << ",\"identities\":" << state->identities
           << ",\"cells\":" << cells_json() << '}';
    output.close();
    if (state->tiled) {
        auto allocated = temp.directory / "allocated.json";
        run_helper({ "allocate", "--context", state->path, "--snapshot",
                     snapshot.u8string(), "--output", allocated.u8string() });
        auto root = parse(read_file(allocated, MAX_STATE));
        State metadata;
        metadata.path = state->path;
        metadata.manifest = state->manifest;
        metadata.tiled = true;
        metadata.center = state->center;
        metadata.bookmarks = state->bookmarks;
        metadata.managed = state->managed;
        metadata.new_objects_solid = state->new_objects_solid;
        metadata.source = string((*root)["source"]);
        metadata.identities = serialize(&(*root)["identities"]);
        saved_data(metadata);
        run_helper({ "export", "--context", state->path,
                     "--snapshot", allocated.u8string(), "--output", path });
        state->source = std::move(metadata.source);
        state->identities = std::move(metadata.identities);
        update_metadata_key(*state);
        status = "Exported all locations to " + std::string(path);
        return;
    }
    run_helper({ "export", "--context", state->path, "--snapshot",
                 snapshot.u8string(), "--output", path });
    status = "Exported Crystal Edit project to " + std::string(path);
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
        float box[4][4];
        view_box(*state, box);
        auto it = state->tiled ?
            volume_get_box_iterator(layer->volume, box,
                                    VOLUME_ITER_SKIP_EMPTY) :
            volume_get_iterator(layer->volume, VOLUME_ITER_SKIP_EMPTY);
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
    helper = path && *path ? path : crystal_host::bundled_helper();
}
extern "C" bool crystal_active(void)
{
    return bool(state);
}

extern "C" bool crystal_reference_bounds(float box[4][4])
{
    if (!state) return false;
    view_box(*state, box);
    return true;
}
extern "C" void crystal_reset(void)
{
    state.reset();
    pending.clear();
    status.clear();
    context_input[0] = '\0';
    selected = variant = 0;
    failed_tile_request.clear();
}

namespace {
bool prepare_area(const float box[4][4], bool edit)
{
    if (!state || !state->tiled || box_is_null(box)) return true;
    std::string request;
    try {
        float corners[8][3];
        box_get_vertices(box, corners);
        std::array<int, 3> lower, upper;
        for (int k = 0; k < 3; k++) {
            float min = corners[0][k], max = corners[0][k];
            for (auto &corner : corners) {
                if (!std::isfinite(corner[k]) ||
                    std::abs(corner[k]) > WORLD_EXTENT + NATIVE_TILE_EDGE)
                    throw std::runtime_error(
                            "Edit exceeds supported world bounds");
                min = std::min(min, corner[k]);
                max = std::max(max, corner[k]);
            }
            lower[k] = int(std::floor(min));
            upper[k] = std::max(lower[k] + 1, int(std::ceil(max)));
        }
        std::array<int, 3> min{ lower[0], lower[2], -upper[1] };
        std::array<int, 3> max{ upper[0], upper[2], -lower[1] };
        if (min[0] < -WORLD_EXTENT || max[0] > WORLD_EXTENT + 1 ||
            min[1] < 0 || max[1] > WORLD_HEIGHT ||
            min[2] < -WORLD_EXTENT || max[2] > WORLD_EXTENT + 1)
            throw std::runtime_error("Edit exceeds supported world bounds");
        request = triple(min) + ":" + triple(max);
        bool ready = true;
        size_t count = 1;
        for (int k = 0; k < 3; k++)
            count *= size_t(tile_index(max[k] - 1) - tile_index(min[k]) + 1);
        if (count > MAX_VIEW_TILES)
            throw std::runtime_error(
                    "Edit exceeds the tile limit; use a smaller selection");
        for (int x = tile_index(min[0]); x <= tile_index(max[0] - 1); x++)
            for (int y = tile_index(min[1]); y <= tile_index(max[1] - 1); y++)
                for (int z = tile_index(min[2]);
                     z <= tile_index(max[2] - 1); z++)
                    ready &= state->tiles.count({ x, y, z }) != 0;
        // Hover cannot replace the reference view or start foreground loading
        if (!edit) return ready;
        if (request == failed_tile_request) return false;
        if (ready) return true;
        auto expanded_min = min, expanded_max = max;
        count = 1;
        for (int k = 0; k < 3; k++) {
            expanded_min[k] = std::min(min[k], state->view_min[k]);
            expanded_max[k] = std::max(max[k], state->view_max[k]);
            if (k == 1) {
                expanded_min[k] = std::max(expanded_min[k], 0);
                expanded_max[k] = std::min(expanded_max[k], WORLD_HEIGHT);
            } else {
                expanded_min[k] = std::max(expanded_min[k], -WORLD_EXTENT);
                expanded_max[k] = std::min(expanded_max[k], WORLD_EXTENT + 1);
            }
            count *= size_t(tile_index(expanded_max[k] - 1) -
                            tile_index(expanded_min[k]) + 1);
        }
        prepare_view(*state, count <= MAX_VIEW_TILES ? expanded_min : min,
                     count <= MAX_VIEW_TILES ? expanded_max : max);
        failed_tile_request.clear();
        return true;
    }
    catch (const std::exception &error) {
        if (!edit) return false;
        failed_tile_request = request;
        status = "Edit cancelled: " + std::string(error.what());
        return false;
    }
}
} // namespace

extern "C" bool crystal_prepare_edit(const float box[4][4])
{
    return prepare_area(box, true);
}

extern "C" bool crystal_prepare_preview(const float box[4][4])
{
    return prepare_area(box, false);
}

namespace {
bool camera_needs_view(const std::array<int, 3> &center);

void start_terrain_job(const std::array<int, 3> &center, bool prefetch)
{
    auto job = std::make_unique<TerrainJob>();
    job->path = state->path;
    job->center = center;
    job->revision = state->view_revision;
    job->document = state->document;
    job->prefetch = prefetch;
    job->started = std::chrono::steady_clock::now();
    std::array<int, 3> min, max;
    if (prefetch) {
        // Center speculation on the visible bounds even after a narrow edit
        for (int k = 0; k < 3; k++)
            job->center[k] = std::clamp(
                    (state->view_min[k] + state->view_max[k]) / 2,
                    k == 1 ? 0 : -WORLD_EXTENT,
                    k == 1 ? WORLD_HEIGHT - 1 : WORLD_EXTENT);
        location_bounds(job->center, PREFETCH_TILE_RADIUS, min, max);
    } else location_bounds(center, VIEW_TILE_RADIUS, min, max);
    auto cache = reusable_tiles(*state);
    auto context = state->path, executable = helper;
    job->result = std::async(std::launch::async,
            [context, executable, min, max, cache, prefetch] {
                return prepare_terrain(context, executable, min, max,
                                       cache, prefetch);
            });
    terrain_job = std::move(job);
    if (prefetch) {
        state->prefetch_attempted = true;
        state->prefetch_error.clear();
    } else status = "Loading terrain. The current view remains available.";
}

void poll_terrain_job(const std::array<int, 3> *camera_center = nullptr)
{
    if (!terrain_job || terrain_job->result.wait_for(
            std::chrono::milliseconds(0)) != std::future_status::ready) return;
    auto job = std::move(terrain_job);
    bool relevant = state && state->document == job->document &&
                    state->path == job->path;
    double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - job->started).count();
    auto discard = [&](const char *reason) {
        fprintf(stderr, "crystal-goxel terrain-job: operation=%s "
                "result=discarded document=%llu revision=%llu "
                "elapsedMs=%.2f reason=%s\n",
                job->prefetch ? "prefetch" : "navigation",
                (unsigned long long)job->document,
                (unsigned long long)job->revision, elapsed, reason);
    };
    try {
        auto prepared = job->result.get();
        if (!relevant) { discard("document-changed"); return; }
        if (job->prefetch) {
            // Speculation cannot replace the visible view or authored state
            for (const auto &entry : prepared.tiles)
                if (!tile_unchanged(entry.second))
                    throw std::runtime_error(
                            "Nearby tile changed before publication");
            cache_terrain(*state, prepared);
        } else if (state->view_revision == job->revision) {
            if (!state->follow_view ||
                (camera_center && !camera_needs_view(*camera_center))) {
                discard("camera-returned-or-follow-disabled");
                status = "Current area ready.";
                return;
            }
            if (camera_center) {
                for (int k = 0; k < 3; k++)
                    if ((*camera_center)[k] < prepared.min[k] ||
                        (*camera_center)[k] >= prepared.max[k]) {
                        discard("camera-moved-beyond-request");
                        return;
                    }
            }
            publish_terrain(*state, prepared);
            state->center = job->center;
            update_metadata_key(*state);
            std::copy(job->center.begin(), job->center.end(), location_input);
            volume_delete(goxel.tool_volume);
            goxel.tool_volume = nullptr;
            failed_tile_request.clear();
            status = "Location loaded. Edits at other locations remain in "
                     "this mod.";
        } else { discard("view-changed"); return; }
        fprintf(stderr, "crystal-goxel terrain-job: operation=%s result=ok "
                "document=%llu revision=%llu location=%s tiles=%zu "
                "cache=%s cachedTiles=%zu cachedBytes=%zu elapsedMs=%.2f\n",
                job->prefetch ? "prefetch" : "navigation",
                (unsigned long long)job->document,
                (unsigned long long)job->revision,
                triple(job->center).c_str(), prepared.tiles.size(),
                prepared.from_memory ? "memory" : "disk",
                state->cache.size(), state->cache_bytes, elapsed);
    }
    catch (const std::exception &error) {
        if (!relevant) { discard("document-changed"); return; }
        if (job->prefetch) {
            state->prefetch_error = "Nearby terrain unavailable: " +
                                    std::string(error.what());
        } else if (state->view_revision == job->revision) {
            if (!state->follow_view) {
                discard("follow-disabled");
                status = "Current area ready.";
                return;
            }
            std::array<int, 3> key;
            for (int k = 0; k < 3; k++) key[k] = tile_index(job->center[k]);
            failed_tile_request = "camera:" + triple(key);
            status = "Terrain loading failed: " + std::string(error.what());
        } else { discard("view-changed"); return; }
        fprintf(stderr, "crystal-goxel terrain-job: operation=%s result=error "
                "document=%llu revision=%llu location=%s elapsedMs=%.2f "
                "message=%s\n", job->prefetch ? "prefetch" : "navigation",
                (unsigned long long)job->document,
                (unsigned long long)job->revision,
                triple(job->center).c_str(), elapsed, error.what());
    }
}

bool camera_needs_view(const std::array<int, 3> &center)
{
    for (int k = 0; k < 3; k++) {
        auto lower = k == 1 ? 0 : -WORLD_EXTENT;
        auto upper = k == 1 ? WORLD_HEIGHT : WORLD_EXTENT + 1;
        if ((state->view_min[k] > lower &&
             center[k] < state->view_min[k] + CAMERA_EDGE_MARGIN) ||
            (state->view_max[k] < upper &&
             center[k] >= state->view_max[k] - CAMERA_EDGE_MARGIN)) return true;
    }
    return false;
}
} // namespace

extern "C" void crystal_follow_view(const camera_t *camera)
{
    if (!state || !state->tiled || !state->follow_view) {
        poll_terrain_job();
        return;
    }
    float target[3];
    mat4_mul_vec3(camera->mat, VEC(0, 0, -camera->dist), target);
    for (float value : target)
        if (!std::isfinite(value) || std::abs(value) > WORLD_EXTENT) return;
    std::array<int, 3> center{
        int(std::floor(target[0])),
        std::clamp(int(std::floor(target[2])), 0, WORLD_HEIGHT - 1),
        int(std::floor(-target[1]))
    };
    poll_terrain_job(&center);
    if (terrain_job) return;
    std::array<int, 3> key;
    for (int k = 0; k < 3; k++) key[k] = tile_index(center[k]);
    auto request = "camera:" + triple(key);
    try {
        if (camera_needs_view(center)) {
            if (request != failed_tile_request)
                start_terrain_job(center, false);
        } else if (!state->prefetch_attempted)
            start_terrain_job(state->center, true);
    }
    catch (const std::exception &error) {
        failed_tile_request = request;
        status = "Terrain loading failed: " + std::string(error.what());
    }
}

extern "C" bool crystal_step_height(int direction)
{
    if (!state || !state->tiled || !goxel.image->active_camera ||
        (direction != -1 && direction != 1)) return false;
    auto *camera = goxel.image->active_camera;
    try {
        float target[3];
        mat4_mul_vec3(camera->mat, VEC(0, 0, -camera->dist), target);
        for (float value : target)
            if (!std::isfinite(value) || std::abs(value) > WORLD_EXTENT)
                throw std::runtime_error("Camera target exceeds world bounds");
        auto step = std::clamp(state->height_step, 1, WORLD_HEIGHT - 1);
        auto height = std::clamp(target[2] + direction * step,
                                 .5f, WORLD_HEIGHT - .5f);
        if (std::abs(height - target[2]) < CAMERA_POSITION_EPSILON) {
            status = direction > 0 ? "Highest world height reached." :
                                     "Lowest world height reached.";
            return false;
        }
        std::array<int, 3> center{
            int(std::floor(target[0])), int(std::floor(height)),
            int(std::floor(-target[1]))
        };
        // Prepare first so a load failure leaves the view and camera intact
        visit_location(center, false);
        // Goxel Z is game Height Y; world translation preserves the orbit pose
        camera->mat[3][2] += height - target[2];
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = nullptr;
        status = "Height loaded. Viewing angle and zoom preserved.";
        return true;
    }
    catch (const std::exception &error) {
        status = "Height change failed: " + std::string(error.what());
        fprintf(stderr, "crystal-goxel height: document=%llu result=error "
                "message=%s\n", (unsigned long long)state->document,
                error.what());
        return false;
    }
}

extern "C" void crystal_height_shortcut(const inputs_t *inputs, bool allowed)
{
    if (!state || !state->tiled || !inputs) return;
    bool up = inputs->keys[KEY_PAGE_UP], down = inputs->keys[KEY_PAGE_DOWN];
    bool pressed = (up && !state->height_up_pressed) ||
                   (down && !state->height_down_pressed);
    // Remember blocked presses so leaving a text field cannot trigger a jump
    state->height_up_pressed = up;
    state->height_down_pressed = down;
    if (!allowed || !pressed || up == down) return;
    for (bool button : inputs->touches[0].down)
        if (button) return;
    if (inputs->keys[KEY_LEFT_CONTROL] || inputs->keys[KEY_RIGHT_CONTROL] ||
        inputs->keys[KEY_LEFT_ALT] || inputs->keys[KEY_RIGHT_ALT] ||
        inputs->keys[KEY_LEFT_SUPER] || inputs->keys[KEY_RIGHT_SUPER] ||
        inputs->keys[KEY_LEFT_SHIFT] || inputs->keys[KEY_RIGHT_SHIFT]) return;
    const auto &pos = inputs->touches[0].pos;
    if (pos[0] < 0 || pos[1] < 0 || pos[0] >= goxel.screen_size[0] ||
        pos[1] >= goxel.screen_size[1]) return;
    crystal_step_height(up ? 1 : -1);
}

extern "C" bool crystal_load(const char *path, bool frame)
{
    try {
        auto next = std::make_unique<State>();
        next->path = std::filesystem::absolute(
                std::filesystem::u8path(path)).u8string();
        next->manifest = read_file(next->path, MAX_STATE);
        auto root = parse(next->manifest);
        run_helper({ "validate", "--context", next->path });
        next->tiled = number((*root)["format"]) == 2;
        next->origin = triple((*root)["origin"]);
        if (!next->tiled) next->size = triple((*root)["size"]);
        bool same_source = state && state->manifest == next->manifest;
        if (state && !same_source &&
            (!state->source.empty() || cells_json() != "[]"))
            throw std::runtime_error(
                    "Start a new document before changing its native source");
        const auto &blocks = (*root)["blocks"];
        if (blocks.type != json_array || !blocks.u.array.length)
            throw std::runtime_error("Missing native palette");
        for (unsigned i = 0; i < blocks.u.array.length; i++) {
            const auto &block = *blocks.u.array.values[i];
            next->blocks.push_back({ number(block["id"]),
                                     number(block["maxVariant"]),
                                     string(block["name"]) });
        }
        auto directory = std::filesystem::u8path(next->path).parent_path();
        auto data = read_file(directory / "palette.mesh");
        size_t offset = 4;
        if (data.substr(0, 4) != "CGP1")
            throw std::runtime_error("Invalid palette mesh signature");
        while (offset < data.size()) {
            auto id = integer(data, offset);
            auto v = integer(data, offset);
            auto count = integer(data, offset);
            if (id < 1 || id > 255 || v < 0 || v > 3 ||
                !next->templates.emplace(id * 4 + v,
                                         vertices(data, offset, count)).second)
                throw std::runtime_error(
                        "Invalid or duplicate native block template");
        }
        int bpp = 4;
        auto atlas_path = (directory / "atlas.png").u8string();
        auto *pixels = img_read(atlas_path.c_str(), &next->atlas_w,
                                &next->atlas_h, &bpp);
        if (!pixels || bpp != 4 || next->atlas_w != 432 ||
            next->atlas_h != 432) {
            free(pixels);
            throw std::runtime_error("Unexpected native atlas dimensions");
        }
        next->pixels.assign(pixels, pixels + 432 * 432 * 4);
        next->atlas = texture_new_from_buf(pixels, 432, 432, 4, TF_NEAREST);
        free(pixels);
        if (same_source) {
            next->source = state->source;
            next->managed = state->managed;
            next->identities = state->identities;
            next->bookmarks = state->bookmarks;
            next->center = state->center;
            next->new_objects_solid = state->new_objects_solid;
        }
        if (!pending.empty()) {
            auto saved = saved_json(pending);
            if (next->manifest != string((*saved)["manifest"]))
                throw std::runtime_error(
                        "Choose the original saved native source");
            restore_metadata(*next, *saved);
        }
        if (next->tiled) {
            location_view(*next, next->center);
            load_native_locations(*next);
        } else {
            std::vector<model_vertex_t> mesh;
            reference_geometry(directory, next->origin, next->size, false,
                               mesh, next->reference_cells);
            next->reference = model(mesh);
        }
        saved_data(*next);
        update_metadata_key(*next);
        state = std::move(next);
        selected = variant = 0;
        choose_block();
        pending.clear();
        float box[4][4];
        view_box(*state, box);
        if (state->tiled)
            memset(goxel.image->box, 0, sizeof(goxel.image->box));
        else
            mat4_copy(box, goxel.image->box);
        goxel.hide_box = true;
        goxel.snap_mask = SNAP_VOLUME;
        if (frame) camera_fit_box(goxel.image->active_camera, box);
        snprintf(context_input, sizeof(context_input), "%s", path);
        std::copy(state->center.begin(), state->center.end(), location_input);
        if (goxel.image->active_layer)
            snprintf(goxel.image->active_layer->name,
                     sizeof(goxel.image->active_layer->name),
                     "Authored Crystal Edit voxels");
        failed_tile_request.clear();
        status = "Native context loaded. Select a block, then paint.";
        if (state->tiled) status += " All locations belong to one mod.";
        return true;
    }
    catch (const std::exception &error) {
        status = error.what();
        fprintf(stderr, "crystal-goxel load: %s\n", error.what());
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
    if (gui_section_begin("Create tiled world",
                          GUI_SECTION_COLLAPSABLE_CLOSED)) {
        gui_input_text("Game installation", installation_input,
                       sizeof(installation_input));
        if (gui_button("Choose game folder", 0, 0)) {
            auto *path = sys_open_folder_dialog(
                    "Select Windows game installation", nullptr);
            if (path) snprintf(installation_input, sizeof(installation_input),
                               "%s", path);
        }
        gui_input_text("New cache folder", cache_input, sizeof(cache_input));
        if (gui_button("Choose cache parent", 0, 0)) {
            auto *path = sys_open_folder_dialog(
                    "Choose a private cache parent", nullptr);
            if (path) snprintf(cache_input, sizeof(cache_input),
                               "%s/crystal-goxel-world", path);
        }
        if (gui_button("Create world cache", 0, 0)) {
            try {
                if (!pending.empty() ||
                    (state && (!state->source.empty() || cells_json() != "[]")))
                    throw std::runtime_error(
                            "Create a world in a new authored document");
                if (!installation_input[0] || !cache_input[0])
                    throw std::runtime_error(
                            "Select the game and a new cache folder");
                run_helper({ "world", "--game", installation_input,
                             "--output", cache_input });
                auto path = std::filesystem::u8path(cache_input) / "world.json";
                crystal_load(path.u8string().c_str(), true);
            }
            catch (const std::exception &error) {
                status = error.what();
            }
        }
        gui_text_wrapped("Choose a new private cache folder. Native terrain is "
                         "prepared as you visit locations.");
    }
    gui_section_end();
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
        {
            float box[4][4];
            view_box(*state, box);
            camera_fit_box(goxel.image->active_camera, box);
        }
        if (state->tiled) {
            try {
                gui_input_int("World X", &location_input[0],
                              -WORLD_EXTENT, WORLD_EXTENT);
                gui_input_int("Height Y", &location_input[1],
                              0, WORLD_HEIGHT - 1);
                gui_input_int("World Z", &location_input[2],
                              -WORLD_EXTENT, WORLD_EXTENT);
                if (gui_button("Go to location", 0, 0))
                    visit_location({ location_input[0], location_input[1],
                                     location_input[2] }, true);
                gui_input_int("Height step", &state->height_step,
                              1, WORLD_HEIGHT - 1);
                gui_row_begin(2);
                bool up = gui_button("Move up", 0, 0);
                bool down = gui_button("Move down", 0, 0);
                gui_row_end();
                if (up || down) crystal_step_height(up ? 1 : -1);
                gui_text_wrapped("Move vertically by the height step in "
                                 "blocks. Page Up / Page Down over the world "
                                 "also move up / down.");
                gui_text_wrapped("Game Height Y is Goxel's vertical Z axis.");
                gui_text("Visible terrain tiles: %zu", state->tiles.size());
                gui_text_wrapped("View: %d x %d x %d blocks", state->size[0],
                                 state->size[1], state->size[2]);
                gui_text("Ready terrain tiles: %zu", state->cache.size());
                if (terrain_job && terrain_job->document == state->document)
                    gui_text(terrain_job->prefetch ? "Preparing nearby terrain"
                                                   : "Loading camera area");
                if (!state->prefetch_error.empty())
                    gui_text_wrapped("%s", state->prefetch_error.c_str());
                gui_checkbox("Follow camera", &state->follow_view,
                             "Prepare terrain when the camera moves");
                gui_text_wrapped("All locations share this mod. Brushes and "
                                 "shapes prepare terrain before editing.");
                if (gui_button("Retry terrain loading", 0, 0)) {
                    failed_tile_request.clear();
                    state->prefetch_error.clear();
                    visit_location(state->center, false);
                }
                native_locations_panel();
                if (gui_section_begin("Saved locations",
                                      GUI_SECTION_COLLAPSABLE_CLOSED)) {
                    gui_input_text("Location name", bookmark_input,
                                   sizeof(bookmark_input));
                    if (gui_button("Save this location", 0, 0)) {
                        add_bookmark(bookmark_input);
                        bookmark_input[0] = '\0';
                    }
                    auto root = parse("{\"items\":" + state->bookmarks + "}");
                    const auto &items = (*root)["items"];
                    for (unsigned i = 0; i < items.u.array.length; i++) {
                        gui_row_begin(2);
                        auto label = string(items[i]["name"]) + "##visit-" +
                                     std::to_string(i);
                        bool visit = gui_button(label.c_str(), 0, 0);
                        label = "Remove##location-" + std::to_string(i);
                        bool remove = gui_button(label.c_str(), 0, 0);
                        gui_row_end();
                        if (visit)
                            visit_location(triple(items[i]["world"]), true);
                        if (remove) {
                            std::string next = "[";
                            for (unsigned j = 0;
                                 j < items.u.array.length; j++) {
                                if (j == i) continue;
                                if (next.size() > 1) next += ',';
                                next += serialize(&items[j]);
                            }
                            auto normalized =
                                    parse("{\"items\":" + next + "]}");
                            state->bookmarks =
                                    serialize(&(*normalized)["items"]);
                            update_metadata_key(*state);
                            break;
                        }
                    }
                }
                gui_section_end();
            }
            catch (const std::exception &error) {
                status = error.what();
            }
        }
        gui_text("World origin: %d, %d, %d", state->origin[0],
                 state->origin[1], state->origin[2]);
        gui_text("Block");
        if (gui_combo_begin("##crystal-block",
                            state->blocks[selected].name.c_str())) {
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
        gui_text("Variant");
        if (gui_combo("##crystal-variant", &variant, variants,
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
                         "preserved. Every location exports together. "
                         "Native terrain is reference only.");
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

namespace {
void wait_terrain_job()
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(30);
    while (terrain_job) {
        poll_terrain_job();
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Terrain smoke job exceeded its deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void native_locations_smoke()
{
    auto path = state->path;
    auto catalog_path = std::filesystem::u8path(path).parent_path() /
                        "locations.json";
    auto original = read_file(catalog_path);
    auto saved = saved_data(*state);
    auto authored = cells_json();
    try {
        std::ofstream(catalog_path) << "{}";
        if (!crystal_load(path.c_str(), false) || !state ||
            state->locations_error.empty() || !state->locations.empty() ||
            cells_json() != authored || saved_data(*state) != saved ||
            !state->reference || !state->reference->nb_vertices)
            throw std::runtime_error(
                    "Unavailable native locations prevented document recovery");
    }
    catch (...) {
        std::ofstream(catalog_path) << original;
        throw;
    }
    std::ofstream(catalog_path) << original;
    load_native_locations(*state);
    if (!state->locations_error.empty() || state->locations.empty() ||
        cells_json() != authored || saved_data(*state) != saved)
        throw std::runtime_error(
                "Native location retry changed authored state");
}

void cache_eviction_smoke()
{
    State saved;
    PreparedTerrain data{};
    for (int i = 0; i < 3; i++) {
        auto tile = std::make_shared<TerrainTile>();
        tile->key = { i, 0, 0 };
        data.tiles[tile->key] = tile;
    }
    cache_terrain(saved, data);
    auto newest = data.tiles.at({ 2, 0, 0 });
    trim_terrain_cache(saved, newest->bytes() * 2, MAX_CACHED_TILES);
    if (saved.cache.size() != 2 || saved.cache.count({ 0, 0, 0 }))
        throw std::runtime_error("Terrain cache did not evict the oldest data");
    trim_terrain_cache(saved, MAX_CACHED_BYTES, 1);
    if (saved.cache.size() != 1 || !saved.cache.count({ 2, 0, 0 }) ||
        saved.cache_bytes != newest->bytes())
        throw std::runtime_error(
                "Terrain cache count or byte accounting failed");
}

void height_navigation_smoke()
{
    auto authored = cells_json(), metadata = saved_data(*state);
    auto center = state->center;
    auto *camera = goxel.image->active_camera;
    float original[4][4];
    mat4_copy(camera->mat, original);
    auto distance = camera->dist;
    auto ortho = camera->ortho, follow = state->follow_view;
    auto step = state->height_step;
    state->follow_view = false;
    state->height_step = NATIVE_TILE_EDGE;
    auto require = [](bool condition, const char *message) {
        if (!condition) throw std::runtime_error(message);
    };
    auto target_height = [&]() {
        float target[3];
        mat4_mul_vec3(camera->mat, VEC(0, 0, -camera->dist), target);
        return target[2];
    };
    for (bool tilted : { false, true }) {
        visit_location({ 1, 99, 1 }, false);
        mat4_set_identity(camera->mat);
        camera->dist = 128;
        camera->ortho = tilted;
        mat4_itranslate(camera->mat, 1.25f, -1.75f, 99.5f + camera->dist);
        if (tilted) camera_turntable(camera, .4f, .6f);
        float pose[4][4], expected[4][4];
        mat4_copy(camera->mat, pose);
        mat4_copy(pose, expected);
        expected[3][2] += NATIVE_TILE_EDGE;
        require(crystal_step_height(1) && mat4_equal(camera->mat, expected) &&
                state->center == std::array<int, 3>{ 1, 115, 1 } &&
                camera->dist == 128 && camera->ortho == tilted &&
                cells_json() == authored && !state->follow_view,
                "Vertical movement lost its world axis or orbit pose");
        require(crystal_step_height(-1) && mat4_equal(camera->mat, pose) &&
                state->center == std::array<int, 3>{ 1, 99, 1 },
                "Vertical return changed horizontal position or zoom");
    }
    state->height_step = 1;
    require(crystal_step_height(1) &&
            state->center == std::array<int, 3>{ 1, 100, 1 } &&
            crystal_step_height(-1), "Fine height navigation lost a cell");
    state->height_step = NATIVE_TILE_EDGE;
    inputs_t input = {};
    input.window_size[0] = 1024; input.window_size[1] = 768;
    input.scale = 1;
    input.touches[0].pos[0] = 800; input.touches[0].pos[1] = 400;
    auto frame = [&]() { goxel_iter(&input); goxel_render(&input); };
    frame(); frame();
    input.keys[KEY_PAGE_UP] = true;
    frame(); frame();
    require(state->center == std::array<int, 3>{ 1, 115, 1 },
            "Page Up failed or repeated while held in the viewport");
    input.keys[KEY_PAGE_UP] = false;
    frame();
    input.keys[KEY_PAGE_DOWN] = true;
    frame(); frame();
    require(state->center == std::array<int, 3>{ 1, 99, 1 },
            "Page Down failed to return through GUI input dispatch");
    input.keys[KEY_PAGE_DOWN] = false;
    crystal_height_shortcut(&input, true);
    input.keys[KEY_PAGE_UP] = true;
    crystal_height_shortcut(&input, false);
    crystal_height_shortcut(&input, true);
    require(state->center == std::array<int, 3>{ 1, 99, 1 },
            "Captured height keys activated after leaving the UI");
    input.keys[KEY_PAGE_UP] = false;
    crystal_height_shortcut(&input, true);
    input.keys[KEY_PAGE_UP] = true;
    input.touches[0].down[0] = true;
    crystal_height_shortcut(&input, true);
    require(state->center == std::array<int, 3>{ 1, 99, 1 },
            "Height shortcut interrupted a mouse gesture");
    input.touches[0].down[0] = false;
    input.keys[KEY_PAGE_UP] = false;
    crystal_height_shortcut(&input, true);
    input.keys[KEY_PAGE_UP] = true;
    input.keys[KEY_LEFT_SUPER] = true;
    crystal_height_shortcut(&input, true);
    require(state->center == std::array<int, 3>{ 1, 99, 1 },
            "Modified Page Up moved the camera");
    input.keys[KEY_PAGE_UP] = false;
    input.keys[KEY_LEFT_SUPER] = false;
    crystal_height_shortcut(&input, true);
    Temporary temporary;
    auto prepared = temporary.directory / "height-tile.json";
    auto ceiling = (tile_index(99) + VIEW_TILE_RADIUS + 1) * NATIVE_TILE_EDGE;
    std::array<int, 3> next{ 1, ceiling, 1 }, end{ 2, ceiling + 1, 2 };
    run_helper({ "tiles", "--context", state->path, "--min", triple(next),
                 "--max", triple(end), "--output", prepared.u8string() });
    auto tile = parse(read_file(prepared, MAX_STATE));
    auto mesh_path =
            std::filesystem::u8path(string((*tile)["tiles"][0]["path"])) /
            "reference.mesh";
    {
        struct RestoreAsset {
            std::filesystem::path path;
            std::string bytes;
            ~RestoreAsset() { std::ofstream(path, std::ios::binary) << bytes; }
        } restore{ mesh_path, read_file(mesh_path) };
        float pose[4][4];
        mat4_copy(camera->mat, pose);
        auto reference = state->reference.get();
        auto saved = saved_data(*state);
        std::ofstream(mesh_path, std::ios::binary | std::ios::app) << "tamper";
        require(!crystal_step_height(1) &&
                status.find("Height change failed") != std::string::npos &&
                mat4_equal(camera->mat, pose) &&
                state->reference.get() == reference &&
                saved_data(*state) == saved && cells_json() == authored,
                "Failed height loading changed the camera or document");
    }
    require(crystal_step_height(1) && crystal_step_height(-1),
            "Height loading did not recover after cache repair");
    state->height_step = WORLD_HEIGHT - 1;
    require(crystal_step_height(-1) &&
            std::abs(target_height() - .5f) < CAMERA_POSITION_EPSILON &&
            state->center[1] == 0 && !crystal_step_height(-1),
            "Height navigation crossed the lower world boundary");
    require(crystal_step_height(1) &&
            std::abs(target_height() - (WORLD_HEIGHT - .5f)) <
                CAMERA_POSITION_EPSILON &&
            state->center[1] == WORLD_HEIGHT - 1 &&
            !crystal_step_height(1),
            "Height navigation crossed the upper world boundary");
    visit_location(center, false);
    mat4_copy(original, camera->mat);
    camera->dist = distance; camera->ortho = ortho;
    state->follow_view = follow; state->height_step = step;
    require(cells_json() == authored && saved_data(*state) == metadata,
            "Height navigation changed authored work or project bookmarks");
}

void benchmark_views()
{
    constexpr int BASELINE_RADIUS = 1;
    constexpr int RENDER_SAMPLES = 24;
    constexpr int PICK_SAMPLES = 128;
    constexpr int WIDTH = 1024;
    constexpr int HEIGHT = 768;
    using Clock = std::chrono::steady_clock;
    auto elapsed = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start)
                .count();
    };
    auto authored = cells_json();
    std::vector<uint8_t> pixels(WIDTH * HEIGHT * 4);
    for (int radius : { BASELINE_RADIUS, VIEW_TILE_RADIUS }) {
        std::array<int, 3> min, max;
        for (int k = 0; k < 3; k++) {
            auto key = tile_index(state->center[k]);
            min[k] = (key - radius) * NATIVE_TILE_EDGE;
            max[k] = (key + radius + 1) * NATIVE_TILE_EDGE;
        }
        auto start = Clock::now();
        prepare_view(*state, min, max);
        double prepare_ms = elapsed(start);
        float box[4][4];
        view_box(*state, box);
        camera_fit_box(goxel.image->active_camera, box);
        goxel_render_to_buf(pixels.data(), WIDTH, HEIGHT, 4);
        // Readback waits for GPU work; this measures static export rendering
        start = Clock::now();
        for (int i = 0; i < RENDER_SAMPLES; i++)
            goxel_render_to_buf(pixels.data(), WIDTH, HEIGHT, 4);
        double render_ms = elapsed(start) / RENDER_SAMPLES;
        float origin[3] = { state->center[0] + .5f,
                            -state->center[2] - .5f,
                            float(state->view_max[1] + 10) };
        float down[3] = { 0, 0, -1 }, hit[3], normal[3];
        start = Clock::now();
        for (int i = 0; i < PICK_SAMPLES; i++)
            if (!pick_ray(origin, down, hit, normal))
                throw std::runtime_error("View benchmark lost floor picking");
        double pick_ms = elapsed(start) / PICK_SAMPLES;
        printf("crystal-goxel view-benchmark: location=%s edge=%d tiles=%zu "
               "nativeVertices=%d prepareMs=%.2f "
               "renderReadback1024x768Ms=%.2f pickMs=%.3f\n",
               triple(state->center).c_str(), state->size[0],
               state->tiles.size(), state->reference->nb_vertices,
               prepare_ms, render_ms, pick_ms);
    }
    if (cells_json() != authored)
        throw std::runtime_error("Changing view size affected authored content");
}

void world_smoke(const char *output)
{
    if (!state->tiled) return;
    wait_terrain_job();
    cache_eviction_smoke();
    height_navigation_smoke();
    state->follow_view = false;
    if (state->size != std::array<int, 3>{ 80, 80, 80 } ||
        state->tiles.size() != MAX_VIEW_TILES)
        throw std::runtime_error("Navigation did not load the full-size view");
    benchmark_views();
    goxel.painter.mode = MODE_OVER;
    goxel.tool_radius = .5f;
    inputs_t input = {};
    input.window_size[0] = 1024;
    input.window_size[1] = 768;
    input.scale = 1;
    input.touches[0].pos[0] = input.touches[0].pos[1] = -1;
    auto frame = [&]() { goxel_iter(&input); goxel_render(&input); };
    auto point = [&](int x, int z, float hit[3]) {
        float origin[3] = { x + .5f, -z - .5f,
                            float(state->view_max[1] + 10) };
        float down[3] = { 0, 0, -1 }, normal[3];
        if (!pick_ray(origin, down, hit, normal))
            throw std::runtime_error(
                    "World smoke could not pick the native floor");
        float screen[3];
        camera_project(goxel.image->active_camera, hit, goxel.gui.viewport,
                       screen);
        input.touches[0].pos[0] = screen[0];
        input.touches[0].pos[1] = input.window_size[1] - screen[1];
    };
    auto release = [&]() {
        input.touches[0].down[0] = false;
        frame(); frame();
        input.touches[0].pos[0] = -1;
        frame();
    };
    auto before = cells_json();
    image_history_push(goxel.image);
    auto *camera = goxel.image->active_camera;
    mat4_set_identity(camera->mat);
    camera->dist = 128;
    camera->ortho = false;
    mat4_itranslate(camera->mat, 8, -8, 104 + camera->dist);
    frame();
    float hit[3];
    point(15, 1, hit); frame(); frame();
    input.touches[0].down[0] = true;
    frame(); frame();
    point(16, 1, hit); frame(); frame();
    release();
    auto seam = cells_json();
    auto cells = parse("{\"cells\":" + seam + "}");
    bool left = false, right = false;
    const auto &items = (*cells)["cells"];
    for (unsigned i = 0; i < items.u.array.length; i++) {
        left |= number(items[i]["pos"][0]) == 15;
        right |= number(items[i]["pos"][0]) == 16;
    }
    if (!left || !right) {
        fprintf(stderr, "world boundary diagnostic: before=%s after=%s "
                        "status=%s\n",
                before.c_str(), seam.c_str(), status.c_str());
        throw std::runtime_error(
                "Mouse stroke did not cross the terrain tile edge");
    }
    image_undo(goxel.image);
    if (cells_json() != before)
        throw std::runtime_error(
                "Boundary stroke did not undo as one operation");
    image_redo(goxel.image);
    if (cells_json() != seam)
        throw std::runtime_error(
                "Boundary stroke redo changed world coordinates");
    action_exec2(ACTION_tool_set_shape);
    auto *brush_shape = goxel.painter.shape;
    goxel.painter.shape = &shape_cube;
    frame();
    point(15, 2, hit); frame(); frame();
    input.touches[0].down[0] = true;
    frame(); frame();
    point(16, 2, hit); frame(); frame();
    release();
    auto shaped = cells_json();
    if (shaped == seam)
        throw std::runtime_error("Boundary shape did not commit");
    image_undo(goxel.image);
    if (cells_json() != seam)
        throw std::runtime_error(
                "Boundary shape did not undo as one operation");
    image_redo(goxel.image);
    if (cells_json() != shaped)
        throw std::runtime_error("Boundary shape redo changed its cells");
    image_undo(goxel.image);
    goxel.painter.shape = brush_shape;
    action_exec2(ACTION_tool_set_brush);
    add_bookmark("Starting area");
    visit_location({ 115, 109, -18 }, true);
    state->follow_view = false;
    add_bookmark("Meadows");
    if (cells_json() != seam)
        throw std::runtime_error("Changing locations lost authored content");
    frame();
    point(115, -18, hit); frame(); frame();
    input.touches[0].down[0] = true;
    frame(); frame();
    release();
    auto complete = cells_json();
    if (complete == seam)
        throw std::runtime_error(
                "Distant location mouse editing did not commit");
    image_undo(goxel.image);
    if (cells_json() != seam)
        throw std::runtime_error("Distant edit undo affected another location");
    image_redo(goxel.image);
    if (cells_json() != complete)
        throw std::runtime_error("Distant edit redo lost a location");
    auto project = std::string(output) + ".world.gox";
    auto center = state->center;
    auto bookmarks = state->bookmarks;
    auto locations = state->locations.size();
    save_to_file(goxel.image, project.c_str());
    if (load_from_file(project.c_str(), true) != 0 ||
        cells_json() != complete || state->center != center ||
        state->bookmarks != bookmarks || state->locations.size() != locations) {
        fprintf(stderr, "world reopen diagnostic: cells=%s expected=%s "
                        "location=%s expectedLocation=%s bookmarks=%s "
                        "expectedBookmarks=%s\n",
                cells_json().c_str(), complete.c_str(),
                triple(state->center).c_str(), triple(center).c_str(),
                state->bookmarks.c_str(), bookmarks.c_str());
        throw std::runtime_error(
                "World document, location or bookmarks did not reopen");
    }
    state->follow_view = false;
    auto exported = std::string(output) + ".world.json";
    export_project(exported.c_str());
    auto first = parse(read_file(exported));
    if ((*first)["locations"].type != json_none ||
        (*first)["bookmarks"].type != json_none)
        throw std::runtime_error(
                "Native navigation entered an authored export");
    auto assignments = state->identities;
    save_to_file(goxel.image, project.c_str());
    if (load_from_file(project.c_str(), true) != 0 ||
        state->identities != assignments || cells_json() != complete)
        throw std::runtime_error(
                "World entity reservations did not survive reopening");
    state->follow_view = false;
    export_project((std::string(output) + ".world-again.json").c_str());
    auto again = parse(read_file(std::string(output) + ".world-again.json"));
    if (serialize(&(*first)["Entities"]) != serialize(&(*again)["Entities"]))
        throw std::runtime_error(
                "Repeated world export changed entity IDs or fields");

    state->follow_view = true;
    camera = goxel.image->active_camera;
    mat4_set_identity(camera->mat);
    camera->dist = 128;
    mat4_itranslate(camera->mat, 1.5f, -1.5f, 99.5f + camera->dist);
    float navigation[4][4];
    mat4_copy(camera->mat, navigation);
    auto reference_before = state->reference.get();
    crystal_follow_view(camera);
    if (!terrain_job || terrain_job->prefetch ||
        state->reference.get() != reference_before)
        throw std::runtime_error(
                "Navigation did not retain the view while loading");
    wait_terrain_job();
    if (state->center != std::array<int, 3>{ 1, 99, 1 } ||
        cells_json() != complete || !mat4_equal(camera->mat, navigation))
        throw std::runtime_error(
                "Camera navigation changed authored content or camera pose");
    reference_before = state->reference.get();
    auto visible = state->tiles;
    auto revision = state->view_revision;
    float preview[4][4];
    bbox_from_extents(preview, VEC(state->view_max[0] + .5f, -1.5f, 99.5f),
                      .5f, .5f, .5f);
    if (crystal_prepare_preview(preview) || state->view_revision != revision ||
        state->reference.get() != reference_before)
        throw std::runtime_error("Unready hover replaced the visible terrain");
    crystal_follow_view(camera);
    if (!terrain_job || !terrain_job->prefetch)
        throw std::runtime_error("Camera did not prepare surrounding terrain");
    wait_terrain_job();
    if (state->cache.size() <= visible.size() || state->tiles != visible ||
        state->reference.get() != reference_before ||
        state->view_revision != revision || !state->prefetch_error.empty())
        throw std::runtime_error(
                "Prefetch changed the view or lost nearby data");
    mat4_itranslate(camera->mat, NATIVE_TILE_EDGE, 0, 0);
    crystal_follow_view(camera);
    if (terrain_job || state->view_revision != revision)
        throw std::runtime_error("Small camera pans reloaded the visible view");
    mat4_itranslate(camera->mat, NATIVE_TILE_EDGE, 0, 0);
    crystal_follow_view(camera);
    wait_terrain_job();
    if (state->center[0] != 1 + 2 * NATIVE_TILE_EDGE)
        throw std::runtime_error(
                "Camera did not extend terrain near the view edge");
    auto memory_views = state->memory_views;
    mat4_copy(navigation, camera->mat);
    crystal_follow_view(camera);
    wait_terrain_job();
    if (state->center != std::array<int, 3>{ 1, 99, 1 } ||
        state->memory_views <= memory_views || cells_json() != complete ||
        state->cache_bytes > MAX_CACHED_BYTES)
        throw std::runtime_error(
                "Returning camera failed to reuse bounded terrain cache");
    mat4_itranslate(camera->mat, 4 * NATIVE_TILE_EDGE, 0, 0);
    crystal_follow_view(camera);
    if (!terrain_job || terrain_job->prefetch)
        throw std::runtime_error("Stale navigation fixture did not start");
    state->follow_view = false;
    visit_location(center, true);
    revision = state->view_revision;
    reference_before = state->reference.get();
    wait_terrain_job();
    if (state->view_revision != revision || state->center != center ||
        state->reference.get() != reference_before || cells_json() != complete)
        throw std::runtime_error(
                "Stale navigation replaced an explicitly selected location");
    state->follow_view = true;
    mat4_copy(navigation, camera->mat);
    crystal_follow_view(camera);
    if (!terrain_job)
        throw std::runtime_error("Document reset fixture did not start a job");
    auto document = state->document;
    crystal_reset();
    if (load_from_file(project.c_str(), true) != 0 ||
        !state || state->document == document)
        throw std::runtime_error("Document reset fixture did not reopen");
    state->follow_view = false;
    reference_before = state->reference.get();
    wait_terrain_job();
    if (state->center != center || cells_json() != complete ||
        state->reference.get() != reference_before)
        throw std::runtime_error(
                "Old document job changed the reopened project");

    // Force a neighboring cache failure after a successful drag segment
    frame();
    point(state->view_max[0] - NATIVE_TILE_EDGE, state->center[2], hit);
    frame(); frame();
    std::array<int, 3> neighbor{
        state->view_max[0], int(std::floor(hit[2])), state->center[2]
    };
    auto end = neighbor;
    for (int k = 0; k < 3; k++) end[k]++;
    Temporary temp;
    auto prepared = temp.directory / "neighbor.json";
    run_helper({ "tiles", "--context", state->path, "--min", triple(neighbor),
                 "--max", triple(end), "--output", prepared.u8string() });
    auto tile = parse(read_file(prepared, MAX_STATE));
    auto mesh_path =
            std::filesystem::u8path(string((*tile)["tiles"][0]["path"])) /
            "reference.mesh";
    struct RestoreAsset {
        std::filesystem::path path;
        std::string bytes;
        ~RestoreAsset() { std::ofstream(path, std::ios::binary) << bytes; }
    } restore{ mesh_path, read_file(mesh_path) };
    std::ofstream(mesh_path, std::ios::binary | std::ios::app) << "tamper";
    auto reference_count = state->reference->nb_vertices;
    auto stable = cells_json();
    input.touches[0].down[0] = true;
    frame(); frame();
    goxel.tool_radius = 32;
    frame(); frame();
    release();
    goxel.tool_radius = .5f;
    if (cells_json() != stable ||
        state->reference->nb_vertices != reference_count ||
        status.find("cancelled") == std::string::npos)
        throw std::runtime_error(
                "Failed boundary loading committed a partial stroke or view");
    failed_tile_request.clear();
    reference_before = state->reference.get();
    start_terrain_job(state->center, true);
    wait_terrain_job();
    if (state->prefetch_error.empty() || cells_json() != stable ||
        state->reference.get() != reference_before)
        throw std::runtime_error(
                "Failed prefetch changed the document or concealed its error");
    std::ofstream(mesh_path, std::ios::binary) << restore.bytes;
    start_terrain_job(state->center, true);
    wait_terrain_job();
    if (!state->prefetch_error.empty() || cells_json() != stable ||
        state->reference.get() != reference_before)
        throw std::runtime_error("Prefetch did not recover after cache repair");
    state->follow_view = true;
    auto pan = [&](int direction) {
        input.touches[0].pos[0] = 800;
        input.touches[0].pos[1] = 400;
        frame(); frame();
        input.touches[0].down[2] = true;
        frame(); frame();
        for (int i = 1; i <= 20; i++) {
            input.touches[0].pos[0] = 800 + direction * i * 20;
            frame();
            if (state->size != std::array<int, 3>{ 80, 80, 80 })
                throw std::runtime_error("Mouse pan collapsed the native view");
        }
        input.touches[0].down[2] = false;
        input.touches[0].pos[0] = -1;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(30);
        do {
            frame();
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error(
                        "Mouse pan loading exceeded its deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (terrain_job);
        if (state->size != std::array<int, 3>{ 80, 80, 80 } ||
            cells_json() != stable || !state->prefetch_error.empty())
            throw std::runtime_error(
                    "Mouse pan changed the document or view size");
    };
    pan(-1);
    pan(1);
    state->follow_view = false;
    visit_location(center, true);
    benchmark_views();
    std::vector<uint8_t> pixels(1024 * 768 * 4);
    goxel_render_to_buf(pixels.data(), 1024, 768, 4);
    img_write(pixels.data(), 1024, 768, 4,
              (std::string(output) + ".world.png").c_str());
    printf("crystal-goxel world smoke: boundaryStroke=ok distantEdit=ok "
           "boundaryShape=ok cameraFollow=ok prefetch=ok hover=ok smallPan=ok "
           "memoryReuse=ok cacheEviction=ok staleNavigation=ok "
           "documentReset=ok prefetchRecovery=ok mousePan=ok "
           "heightNavigation=ok "
           "undoRedo=ok bookmarks=ok nativeLocations=ok "
           "persistence=ok combinedExport=ok "
           "stableIdentities=ok failedStrokeAtomicity=ok\n");
}
} // namespace

extern "C" int crystal_smoke(const char *output)
{
    try {
        if (!state)
            throw std::runtime_error("Smoke check requires --crystal-context");
        if (state->tiled) {
            if (state->locations.empty() || !state->locations_error.empty() ||
                state->bookmarks != "[]")
                throw std::runtime_error(
                        "Fresh native context requires destinations "
                        "without bookmarks");
            auto location = std::find_if(state->locations.begin(),
                                        state->locations.end(),
                    [](const NativeLocation &point) {
                        return point.world ==
                               std::array<int, 3>{ 115, 109, -18 };
                    });
            if (location == state->locations.end() ||
                !location_matches(*location, "sPaWnInG") ||
                !location_matches(*location, "watering") ||
                location_matches(*location, "nonexistent fixture place"))
                throw std::runtime_error(
                        "Native destination search lost its source identity");
            auto authored = cells_json();
            auto saved = saved_data(*state);
            auto spawn = std::find_if(state->locations.begin(),
                                     state->locations.end(),
                    [](const NativeLocation &point) {
                        return point.kind == TELEPORT_POINT_KIND &&
                               point.id == 0;
                    });
            if (spawn == state->locations.end() ||
                !location_matches(*spawn, "spawn") ||
                spawn->world != std::array<int, 3>{ 1, 110, 1 })
                throw std::runtime_error("Native spawn landmark is missing");
            visit_location(spawn->world, true);
            if (state->center != spawn->world || state->bookmarks != "[]" ||
                cells_json() != authored)
                throw std::runtime_error(
                        "Native spawn navigation changed authored state");
            visit_location(location->world, true);
            if (cells_json() != authored || state->bookmarks != "[]")
                throw std::runtime_error(
                        "Native navigation changed authored content "
                        "or bookmarks");
            visit_location({ 1, 99, 1 }, true);
            if (saved_data(*state) != saved)
                throw std::runtime_error(
                        "Native catalog entered saved project metadata");
            native_locations_smoke();
        }
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
        if (state->tiled) {
            origin[0] = state->center[0] + .5f;
            origin[1] = -state->center[2] - .5f;
            origin[2] = state->view_max[1] + 10;
            state->follow_view = false;
        }
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

        auto unavailable = saved;
        auto context_field = "\"context\":" + quote(context_path);
        unavailable.replace(unavailable.find(context_field),
                            context_field.size(),
                            "\"context\":" + quote(source_path + ".missing"));
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
        world_smoke(output);
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
