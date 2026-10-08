// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json.Nodes;

namespace CrystalBridge;

internal static class WorldProjectTests
{
    internal static void Run(string directory)
    {
        void Require(bool condition, string label)
        {
            if (!condition) throw new InvalidDataException("World document test failed: " + label);
        }
        var root = Path.Combine(directory, "world-document");
        var context = TileTests.Fixture(root);
        TileTests.FixtureTile(root, context, [-1, 6, 0], 3);
        TileTests.FixtureTile(root, context, [0, 6, 0], 7);
        TileTests.FixtureTile(root, context, [62, 6, -63], 9);
        var manifest = Path.Combine(root, "world.json");
        var source = Projects.ReadJsonForTests("""
{"ID":"one-mod","EditorVersion":34,"Future":{"number":9007199254740993},"Entities":[{"ID":5000,"EntityType":0,"Coord":{"X":-1,"Y":99,"Z":0},"NpcData":{"Outfits":[{"VoxelID":1,"WanderType":0,"MountType":16,"FutureOutfit":"keep"}],"Pages":[]}},{"ID":6000,"EntityType":0,"Coord":{"X":1000,"Y":99,"Z":-1000},"FutureEntity":"remote","NpcData":{"Outfits":[{"VoxelID":1,"WanderType":0}],"Pages":[]}},{"ID":9000,"EntityType":5,"FutureOther":"untouched"}],"Tree":[{"ModelTypeID":18,"ModelID":12000,"FutureTree":"reserve"}]}
""");
        var input = Path.Combine(root, "source.json");
        File.WriteAllText(input, source.ToJsonString());
        var snapshotPath = Path.Combine(root, "snapshot.json");
        Projects.Import(manifest, input, snapshotPath);
        var snapshot = Projects.Read(snapshotPath);
        Require(snapshot["cells"]!.AsArray().Count == 2 && snapshot["managed"]!.AsArray().Count == 2,
            "distant entities import into one global document");
        snapshot["cells"]!.AsArray().Add(new JsonObject { ["pos"] = new JsonArray(0, -1, 99), ["type"] = 1, ["variant"] = 1 });
        File.WriteAllText(snapshotPath, snapshot.ToJsonString());
        var allocatedPath = Path.Combine(root, "allocated.json");
        Projects.Allocate(manifest, snapshotPath, allocatedPath);
        var allocated = Projects.Read(allocatedPath);
        Require(allocated["identities"]![0]!["id"]!.GetValue<int>() == 12001,
            "ID allocator reserves the entire source and editor tree");
        var first = Path.Combine(root, "first.json");
        Projects.Export(manifest, allocatedPath, first);
        var exported = Projects.Read(first);
        Require(exported["Entities"]!.AsArray().Count == 4 &&
            exported["ID"]!.GetValue<string>() == "one-mod" &&
            exported["Future"]!["number"]!.ToJsonString() == "9007199254740993",
            "combined export retains source identity and exact fields");
        Require(JsonNode.DeepEquals(exported["Entities"]!.AsArray().Single(e => e!["ID"]!.GetValue<int>() == 6000),
            source["Entities"]![1]), "offscreen location survives unchanged");
        Require(exported["Entities"]!.AsArray().Single(e => e!["ID"]!.GetValue<int>() == 12001)!["BiomeID"]!.GetValue<int>() == 7,
            "new entity uses its owning tile's native biome");
        allocated["cells"]!.AsArray().Add(new JsonObject { ["pos"] = new JsonArray(-2, -1, 99), ["type"] = 1, ["variant"] = 0 });
        File.WriteAllText(allocatedPath, allocated.ToJsonString());
        var reallocatedPath = Path.Combine(root, "reallocated.json");
        Projects.Allocate(manifest, allocatedPath, reallocatedPath);
        var second = Path.Combine(root, "second.json");
        Projects.Export(manifest, reallocatedPath, second);
        var entities = Projects.Read(second)["Entities"]!.AsArray();
        Require(entities.Single(e => e!["Coord"]?["X"]?.GetValue<int>() == 0)!["ID"]!.GetValue<int>() == 12001 &&
            entities.Single(e => e!["Coord"]?["X"]?.GetValue<int>() == -2)!["ID"]!.GetValue<int>() == 12002,
            "adding an earlier coordinate never renumbers existing construction");
        var deleted = Projects.Read(reallocatedPath);
        deleted["cells"]!.AsArray().RemoveAt(0);
        File.WriteAllText(reallocatedPath, deleted.ToJsonString());
        var deletion = Path.Combine(root, "deleted.json");
        Projects.Export(manifest, reallocatedPath, deletion);
        Require(Projects.Read(deletion)["Entities"]!.AsArray().All(e => e!["ID"]!.GetValue<int>() != 5000) &&
            Projects.Read(deletion)["Entities"]!.AsArray().Any(e => e!["ID"]!.GetValue<int>() == 6000),
            "explicit local deletion preserves the distant managed entity");
        var conflict = (JsonObject)deleted.DeepClone();
        conflict["identities"]![0]!["id"] = 9000;
        File.WriteAllText(reallocatedPath, conflict.ToJsonString());
        var invalid = Path.Combine(root, "invalid.json"); var rejected = false;
        try { Projects.Export(manifest, reallocatedPath, invalid); }
        catch (InvalidDataException) { rejected = true; }
        Require(rejected && !File.Exists(invalid), "global identity conflicts fail before publishing export");
        var fresh = (JsonObject)snapshot.DeepClone();
        fresh["source"] = ""; fresh["managed"] = new JsonArray();
        File.WriteAllText(snapshotPath, fresh.ToJsonString());
        var freshPath = Path.Combine(root, "fresh-allocated.json");
        Projects.Allocate(manifest, snapshotPath, freshPath);
        Require(!string.IsNullOrEmpty(Projects.Read(freshPath)["source"]!.GetValue<string>()),
            "fresh mod identity is persisted with its first allocations");
    }
}
