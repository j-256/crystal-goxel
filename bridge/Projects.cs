// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CrystalBridge;

internal static class Projects
{
    private const string Marker = "Crystal Goxel voxel";
    private const int MaxJsonBytes = 16 * 1024 * 1024;
    // The inspected NPC free-movement preset has no gravity in air or liquid
    private const int FixedMount = 20;
    private const int FullCollision = 3;
    private const int NoCollision = 0;

    internal static JsonObject Read(string path) => Parse(File.ReadAllText(path));
    internal static JsonObject ReadJsonForTests(string text) => Parse(text);
    private static JsonObject Parse(string text)
    {
        if (System.Text.Encoding.UTF8.GetByteCount(text) > MaxJsonBytes) throw new InvalidDataException("Project JSON exceeds its limit");
        using var doc = JsonDocument.Parse(text);
        Check(doc.RootElement);
        return JsonNode.Parse(text) as JsonObject ?? throw new InvalidDataException("Expected a JSON object");
    }

    private static void Check(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var keys = new HashSet<string>(StringComparer.Ordinal);
            foreach (var field in element.EnumerateObject())
            {
                if (!keys.Add(field.Name)) throw new InvalidDataException("Duplicate JSON field: " + field.Name);
                Check(field.Value);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array) foreach (var value in element.EnumerateArray()) Check(value);
    }

    internal static void Import(string contextPath, string input, string output)
    {
        var context = Read(contextPath);
        ValidateContext(contextPath, context);
        var source = File.ReadAllText(input);
        var project = Parse(source);
        ValidateProject(project);
        var cells = new JsonArray(); var managed = new JsonArray(); var skipped = 0;
        var overlaps = new Dictionary<string, int>();
        foreach (var entity in project["Entities"]!.AsArray())
        {
            if (entity?["NpcData"]?["Outfits"] is not JsonArray outfits || !outfits.Any(o => o?["VoxelID"] is not null)) continue;
            var coord = entity["Coord"];
            if (coord is null) continue;
            var key = string.Join(',', new[] { coord["X"]!.GetValue<int>(), coord["Y"]!.GetValue<int>(), coord["Z"]!.GetValue<int>() });
            overlaps[key] = overlaps.GetValueOrDefault(key) + 1;
        }
        foreach (var node in (JsonArray)project["Entities"]!)
        {
            var entity = (JsonObject)node!;
            if (!StaticVoxel(entity, out var id, out var variant)) { skipped++; continue; }
            var coord = entity["Coord"]!;
            var world = new[] { coord["X"]!.GetValue<int>(), coord["Y"]!.GetValue<int>(), coord["Z"]!.GetValue<int>() };
            if (!Inside(world, context) || !Block(context, id, variant) || overlaps.GetValueOrDefault(string.Join(',', world)) > 1) { skipped++; continue; }
            var editor = Context.ToEditor(world, Ints(context["origin"]!));
            cells.Add(new JsonObject { ["pos"] = JsonSerializer.SerializeToNode(editor), ["type"] = id, ["variant"] = variant });
            managed.Add(new JsonObject { ["id"] = entity["ID"]!.DeepClone(), ["world"] = JsonSerializer.SerializeToNode(world) });
        }
        WriteNew(output, new JsonObject { ["format"] = 1, ["source"] = source, ["managed"] = managed, ["cells"] = cells });
        Console.WriteLine(JsonSerializer.Serialize(new { imported = cells.Count, preserved = skipped, snapshot = Path.GetFullPath(output) }));
    }

    internal static bool StaticVoxel(JsonObject entity, out int id, out int variant)
    {
        id = variant = 0;
        if (entity["EntityType"]?.GetValue<int>() != 0 || entity["NpcData"] is not JsonObject npc) return false;
        if (npc["LinkedKey"] is JsonValue linked && !string.IsNullOrEmpty(linked.GetValue<string>()) || npc["TieToSpawn"]?.GetValue<bool>() == true) return false;
        if (npc["Outfits"] is not JsonArray { Count: 1 } outfits || outfits[0] is not JsonObject outfit || outfit["VoxelID"] is null) return false;
        if (!Always(outfit["Condition"]) || outfit["WanderType"]?.GetValue<int>() is not (null or 0)) return false;
        if (npc["Pages"] is JsonArray pages && pages.Any(p => !Always(p?["Condition"]) || p?["Actions"] is JsonArray { Count: > 0 })) return false;
        id = outfit["VoxelID"]!.GetValue<int>(); variant = outfit["VoxelVariantIndex"]?.GetValue<int>() ?? 0;
        return true;
    }

    private static bool Always(JsonNode? condition) => condition is null || condition is JsonObject value && value["ConditionType"]?.GetValue<int>() == 0 && value["IsNegation"]?.GetValue<bool>() != true && value["Data"] is null;

    internal static void Export(string contextPath, string input, string output)
    {
        var context = Read(contextPath);
        ValidateContext(contextPath, context);
        var snapshot = Read(input);
        if (snapshot["format"]?.GetValue<int>() != 1) throw new InvalidDataException("Unsupported snapshot format");
        var solid = true;
        if (snapshot.TryGetPropertyValue("newObjectsSolid", out var option) && (option is not JsonValue value || !value.TryGetValue(out solid)))
            throw new InvalidDataException("newObjectsSolid must be boolean");
        var raw = snapshot["source"]?.GetValue<string>();
        var project = string.IsNullOrEmpty(raw) ? NewProject() : Parse(raw);
        ValidateProject(project);
        var entities = (JsonArray)project["Entities"]!;
        var managed = new Dictionary<string, JsonObject>();
        var ids = new HashSet<int>();
        foreach (var entity in entities) ids.Add(entity!["ID"]!.GetValue<int>());
        var managedIDs = new HashSet<int>();
        foreach (var item in snapshot["managed"]?.AsArray() ?? [])
        {
            var id = item!["id"]!.GetValue<int>();
            var entity = entities.OfType<JsonObject>().SingleOrDefault(e => e["ID"]!.GetValue<int>() == id) ?? throw new InvalidDataException("Managed entity no longer exists");
            var coord = entity["Coord"];
            var world = Ints(item["world"]!);
            if (!StaticVoxel(entity, out _, out _) || coord is null || !world.SequenceEqual(new[] { coord["X"]!.GetValue<int>(), coord["Y"]!.GetValue<int>(), coord["Z"]!.GetValue<int>() }) || !Inside(world, context) || !managedIDs.Add(id) || !managed.TryAdd(string.Join(',', world), entity)) throw new InvalidDataException("Invalid managed entity mapping");
        }
        var replacement = new List<JsonObject>();
        var occupied = new HashSet<string>();
        var biomes = File.ReadAllBytes(Path.Combine(Path.GetDirectoryName(contextPath)!, "biomes.bin"));
        var size = Ints(context["size"]!); var origin = Ints(context["origin"]!);
        // Reserve entity IDs referenced by the editor tree too, including nodes inside folders
        var treeEntityIDs = project["Tree"] is JsonArray sourceTree ? TreeIDs(sourceTree) : [];
        var next = (long)Math.Max(NativeGame.LastVanillaEntity, ids.Concat(treeEntityIDs).DefaultIfEmpty(0).Max()) + 1;
        foreach (var cell in snapshot["cells"]?.AsArray() ?? throw new InvalidDataException("Missing cells"))
        {
            var pos = Ints(cell!["pos"]!);
            var world = Context.ToWorld(pos, origin);
            var type = cell["type"]!.GetValue<int>(); var variant = cell["variant"]!.GetValue<int>();
            if (!Inside(world, context) || !Block(context, type, variant)) throw new InvalidDataException("Authored cell is outside its context or has an unsupported block identity");
            var key = string.Join(',', world);
            if (!occupied.Add(key)) throw new InvalidDataException("Duplicate authored cell");
            var local = world.Zip(origin, (w, o) => w - o).ToArray();
            var biome = biomes[(local[0] * size[1] + local[1]) * size[2] + local[2]];
            JsonObject entity;
            if (managed.TryGetValue(key, out var previous)) entity = (JsonObject)previous.DeepClone();
            else
            {
                if (next > int.MaxValue) throw new InvalidDataException("Entity IDs exhausted");
                entity = NewEntity((int)next++, world, biome, solid);
            }
            var outfit = entity["NpcData"]!["Outfits"]![0]!;
            outfit["VoxelID"] = type; outfit["VoxelVariantIndex"] = variant;
            replacement.Add(entity);
        }
        var removed = managed.Values.Select(e => e["ID"]!.GetValue<int>()).ToHashSet();
        for (var i = entities.Count - 1; i >= 0; i--) if (removed.Contains(entities[i]!["ID"]!.GetValue<int>())) entities.RemoveAt(i);
        foreach (var entity in replacement) entities.Add(entity);
        if (project["Tree"] is not JsonArray tree) project["Tree"] = tree = [];
        var retained = replacement.Select(e => e["ID"]!.GetValue<int>()).ToHashSet();
        PruneTree(tree, removed.Except(retained).ToHashSet());
        // Crystal Edit's world entities live outside its model tree; existing tree metadata stays intact
        WriteNew(output, project);
        Console.WriteLine(JsonSerializer.Serialize(new { authored = replacement.Count, entities = entities.Count, output = Path.GetFullPath(output) }));
    }

    private static IEnumerable<int> TreeIDs(JsonArray tree)
    {
        foreach (var node in tree)
        {
            if (node?["ModelTypeID"]?.GetValue<int>() == 18 && node["ModelID"] is not null) yield return node["ModelID"]!.GetValue<int>();
            if (node?["Children"] is JsonArray children) foreach (var id in TreeIDs(children)) yield return id;
        }
    }

    private static void PruneTree(JsonArray tree, HashSet<int> removed)
    {
        for (var i = tree.Count - 1; i >= 0; i--)
        {
            var node = tree[i];
            if (node?["ModelTypeID"]?.GetValue<int>() == 18 && removed.Contains(node["ModelID"]!.GetValue<int>())) tree.RemoveAt(i);
            else if (node?["Children"] is JsonArray children) PruneTree(children, removed);
        }
    }

    private static void ValidateProject(JsonObject project)
    {
        if (project["EditorVersion"]?.GetValue<int>() != NativeGame.EditorVersion || string.IsNullOrWhiteSpace(project["ID"]?.GetValue<string>())) throw new InvalidDataException("Expected a Crystal Edit format 34 source project with an ID");
        project["Entities"] ??= new JsonArray();
        var ids = new HashSet<int>();
        foreach (var entity in project["Entities"]!.AsArray())
            if (entity is not JsonObject || entity["ID"] is null || !ids.Add(entity["ID"]!.GetValue<int>())) throw new InvalidDataException("Invalid or duplicate entity ID");
    }

    internal static void ValidateContext(string path, JsonObject context)
    {
        if (TiledContext.IsTiled(context)) { TiledContext.Validate(path, context); return; }
        if (context["format"]?.GetValue<int>() != 1 || context["executableSha256"]?.GetValue<string>() != NativeGame.ReviewedExecutable || context["worldSha256"]?.GetValue<string>() != NativeGame.ReviewedWorld || context["voxelSha256"]?.GetValue<string>() != NativeGame.ReviewedVoxels) throw new InvalidDataException("Unsupported native context");
        var size = Ints(context["size"]!); var origin = Ints(context["origin"]!);
        if (size.Any(v => v < 8 || v > 96) || (long)size[0] * size[1] * size[2] > 300_000) throw new InvalidDataException("Invalid context bounds");
        if (origin.Any(v => v < -10100 || v > 10100) || origin[1] < -48 || origin[1] + size[1] > 304) throw new InvalidDataException("Context origin exceeds native bounds");
        foreach (var name in new[] { "reference.mesh", "reference.cells", "palette.mesh", "atlas.png", "biomes.bin" })
            if (NativeGame.Hash(File.ReadAllBytes(Path.Combine(Path.GetDirectoryName(path)!, name))) != context["files"]?[name]?.GetValue<string>()) throw new InvalidDataException("Context fingerprint mismatch: " + name);
        if (new FileInfo(Path.Combine(Path.GetDirectoryName(path)!, "biomes.bin")).Length != size[0] * size[1] * size[2]) throw new InvalidDataException("Invalid biome volume length");
    }

    private static bool Inside(int[] world, JsonObject context)
    {
        var origin = Ints(context["origin"]!); var size = Ints(context["size"]!);
        return Enumerable.Range(0, 3).All(i => world[i] >= origin[i] && world[i] < origin[i] + size[i]);
    }
    private static bool Block(JsonObject context, int type, int variant)
    {
        var block = context["blocks"]!.AsArray().SingleOrDefault(b => b!["id"]!.GetValue<int>() == type);
        return block is not null && variant >= 0 && variant <= block["maxVariant"]!.GetValue<int>();
    }
    private static int[] Ints(JsonNode node) => node.AsArray().Select(v => v!.GetValue<int>()).ToArray() is { Length: 3 } result ? result : throw new InvalidDataException("Expected three integer coordinates");
    private static JsonObject NewProject()
    {
        var project = new JsonObject
        {
            ["ID"] = Guid.NewGuid().ToString(),
            ["Title"] = "Crystal Goxel construction",
            ["Description"] = "Voxel construction made with Crystal Goxel",
            ["Author"] = "",
            ["Version"] = "1.0",
            ["EditorVersion"] = NativeGame.EditorVersion,
            ["SteamWorkshopFileID"] = 0,
            ["Timestamp"] = DateTime.UtcNow.ToString("O"),
            ["IsLocalization"] = false,
            ["Language"] = "English",
            ["HasCustomContent"] = false,
            ["Entities"] = new JsonArray(),
            ["Tree"] = new JsonArray(),
            ["Folders"] = new JsonArray()
        };
        foreach (var family in new[] { "Abilities", "Animations", "Biomes", "Difficulties", "Equipment", "Genders", "Items", "Jobs", "Monsters", "Passives", "Recipes", "Sparks", "Statuses", "Troops" }) project[family] = new JsonArray();
        return project;
    }
    private static JsonObject NewEntity(int id, int[] world, byte biome, bool solid) => new()
    {
        ["ID"] = id,
        ["Coord"] = new JsonObject { ["X"] = world[0], ["Y"] = world[1], ["Z"] = world[2] },
        ["BiomeID"] = biome,
        ["EntityType"] = 0,
        ["Comments"] = Marker,
        ["NpcData"] = new JsonObject
        {
            ["Key"] = "CrystalGoxel_" + id,
            ["LinkedKey"] = "",
            ["TieToSpawn"] = false,
            ["UniquePerKey"] = false,
            ["Outfits"] = new JsonArray(new JsonObject
            {
                ["Condition"] = new JsonObject { ["ConditionType"] = 0, ["Data"] = null, ["IsNegation"] = false },
                ["TextureKey"] = null,
                ["Name"] = "",
                ["Facing"] = 5,
                ["ShadowType"] = 0,
                ["AutoStep"] = false,
                ["VoxelID"] = 1,
                ["VoxelVariantIndex"] = 0,
                // Native terrain Collides is independent of this NPC's construction collision
                ["PlayerCollision"] = solid ? FullCollision : NoCollision,
                ["NpcCollision"] = solid ? FullCollision : NoCollision,
                ["MountType"] = FixedMount,
                ["JumpType"] = 0,
                ["KeepSpawned"] = true,
                ["WanderType"] = 0,
                ["WanderSpeed"] = 0,
                ["WanderFrequency"] = 0,
                ["WanderRadius"] = 0,
                ["WanderRoute"] = new JsonArray(),
                ["WanderRouteIsLine"] = false
            }),
            ["Pages"] = new JsonArray()
        }
    };
    private static void WriteNew(string path, JsonObject value)
    {
        // Creating a new file prevents an export from overwriting the user's source project
        using var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write);
        JsonSerializer.Serialize(output, value, Context.JsonOptions);
    }

    internal static void Test()
    {
        void Require(bool condition, string label) { if (!condition) throw new InvalidDataException("Test failed: " + label); }
        foreach (var origin in new[] { new[] { -17, 93, -42 }, new[] { 91, 93, -42 } })
            foreach (var point in new[] { new[] { -18, 108, -17 }, new[] { 115, 109, -18 } })
                Require(Context.ToWorld(Context.ToEditor(point, origin), origin).SequenceEqual(point), "negative coordinate round trip");
        var project = Parse("{\"ID\":\"synthetic\",\"EditorVersion\":34,\"SteamWorkshopFileID\":18446744073709551614,\"Future\":{\"quantity\":9007199254740993},\"Entities\":[]}");
        ValidateProject(project);
        Require(project.ToJsonString().Contains("18446744073709551614") && project.ToJsonString().Contains("9007199254740993"), "exact large numeric preservation");
        var duplicateRejected = false;
        try { Parse("{\"ID\":1,\"ID\":2}"); } catch (InvalidDataException) { duplicateRejected = true; }
        Require(duplicateRejected, "duplicate fields rejected");
        var entity = NewEntity(4000, [1, 2, -3], 1, true);
        Require(StaticVoxel(entity, out _, out _), "static generated voxel import");
        entity["NpcData"]!["Outfits"]![0]!["WanderType"] = 1;
        Require(!StaticVoxel(entity, out _, out _), "moving NPC preserved outside authored cells");
        SyntheticTests.Run();
        Console.WriteLine("Synthetic coordinate, identity, preservation, and unsupported-state checks passed");
    }
}
