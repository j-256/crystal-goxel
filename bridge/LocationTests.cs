// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CrystalBridge;

internal static class LocationTests
{
    private static void Require(bool condition, string label)
    {
        if (!condition) throw new InvalidDataException("Location test failed: " + label);
    }

    internal static void Run(string directory)
    {
        var root = Path.Combine(directory, "locations-contract");
        var context = TileTests.Fixture(root);
        var contextPath = Path.Combine(root, "world.json");
        var manifest = File.ReadAllText(contextPath);
        var biomes = JsonSerializer.Deserialize<JsonElement[]>("""
            [{"ID":1,"Name":"Fixture Valley"},{"ID":2,"Name":"Fixture Ridge"}]
            """)!;
        NativeLandmark[] points =
        [
            new(30, "@Z2.Name Camp", 1, [-17, 60, 8]),
            new(12, "Fixture Hut", 1, [0, 99, 1]),
            new(40, "@C@Unknown.Name's Shrine", 2, [16, 255, -16])
        ];
        var identity = TiledContext.Identity(context);
        var catalog = NativeLocations.Build(points, biomes, identity);
        var entries = catalog["entries"]!.AsArray();
        Require(entries[0]!["id"]!.GetValue<int>() == 12 &&
            entries[1]!["name"]!.GetValue<string>() == "Fixture Ridge Camp" &&
            entries[1]!["area"]!.GetValue<string>() == "Fixture Valley" &&
            entries[1]!["world"]!.ToJsonString() == "[-17,60,8]",
            "source identity and exact coordinates survive zone-name resolution");
        Require(!entries[2]!["nameResolved"]!.GetValue<bool>() &&
            entries[2]!["sourceName"]!.GetValue<string>() == points[2].Name,
            "unsupported text variables remain explicit without guessed names");
        var cache = Path.Combine(root, NativeLocations.FileName);
        File.WriteAllText(cache, catalog.ToJsonString(Context.JsonOptions));
        Require(JsonNode.DeepEquals(NativeLocations.Load(contextPath), catalog),
            "cached locations work offline in a fresh world without a project");
        var output = Path.Combine(root, "locations-export.json");
        NativeLocations.Export(contextPath, output);
        Require(JsonNode.DeepEquals(Projects.Read(output), catalog) &&
            File.ReadAllText(contextPath) == manifest,
            "sidecar navigation leaves saved world manifest identity unchanged");
        var protectedOutput = File.ReadAllText(output);
        var rejected = false;
        try { NativeLocations.Export(contextPath, output); } catch (IOException) { rejected = true; }
        Require(rejected && File.ReadAllText(output) == protectedOutput, "catalog output never overwrites a file");

        using var system = JsonDocument.Parse("""
            {"TeleportPoints":[{"Name":"Fixture Spawn","Coord":{"X":3,"Y":11,"Z":-5}},
            {"Name":"Dynamic Home","Coord":{"X":3,"Y":11,"Z":-5}},
            {"Name":"Fixture Race","Coord":{"X":-8,"Y":60,"Z":16}},null]}
            """);
        var teleports = NativeLocations.TeleportPoints(system.RootElement, _ => 2).ToArray();
        Require(teleports.Length == 2 && teleports[0].ID == 0 && teleports[0].World.SequenceEqual([3, 11, -5]) &&
            teleports[1].ID == 2 && teleports.All(p => p.Kind == NativeLocations.TeleportPointKind),
            "non-crystal landmarks keep their native indices and omit dynamic home and empty slots");
        var combined = NativeLocations.Build(points.Concat(teleports).Append(points[0] with { ID = 2 }), biomes, identity);
        Require(combined["entries"]!.AsArray().Count(e => e!["id"]!.GetValue<int>() == 2) == 2,
            "home points and landmarks use distinct source identity namespaces");
        var legacy = (JsonObject)catalog.DeepClone();
        legacy["format"] = 1; legacy.Remove("systemSha256"); legacy.Remove("hasLandmarks");
        foreach (var point in legacy["entries"]!.AsArray()) point!.AsObject().Remove("kind");
        legacy["entriesSha256"] = NativeGame.Hash(System.Text.Encoding.UTF8.GetBytes(legacy["entries"]!.ToJsonString()));
        File.WriteAllText(cache, legacy.ToJsonString());
        var legacyBytes = File.ReadAllBytes(cache);
        var offline = NativeLocations.Load(contextPath);
        Require(!offline["hasLandmarks"]!.GetValue<bool>() && offline["warning"] is not null &&
            offline["entries"]!.AsArray().All(p => p!["kind"]!.GetValue<string>() == NativeLocations.HomePointKind) &&
            File.ReadAllBytes(cache).SequenceEqual(legacyBytes),
            "legacy cached home points remain usable offline with an explicit pending landmark upgrade");
        File.WriteAllText(cache, catalog.ToJsonString());

        void Reject(IEnumerable<NativeLandmark> invalid, string label)
        {
            var failed = false;
            try { NativeLocations.Build(invalid, biomes, identity); }
            catch (InvalidDataException) { failed = true; }
            Require(failed, label);
        }
        Reject([points[0], points[0]], "duplicate source identities are rejected");
        Reject([points[0] with { World = [0, 256, 0] }], "vertical bounds are enforced");
        Reject([points[0] with { Biome = 9 }], "unknown source biomes are rejected");
        Reject([points[0] with { Name = "" }], "empty location names are rejected");
        Reject([points[0] with { Name = "line\nbreak" }], "control characters are rejected");
        Reject(Enumerable.Range(1, NativeLocations.MaxEntries + 1)
            .Select(id => new NativeLandmark(id, "Fixture", 1, [0, 0, 0])), "catalog growth is bounded");
        Reject(Enumerable.Range(1, NativeLocations.MaxEntries)
            .Select(id => new NativeLandmark(id, new string('\u2603', 256), 1, [0, 0, 0])),
            "encoded catalog bytes are bounded before publication");
        rejected = false;
        try { NativeLocations.Validate(catalog, "other-world-identity"); }
        catch (InvalidDataException) { rejected = true; }
        Require(rejected, "catalogs cannot bind to a different native context");
        var changedSystem = (JsonObject)catalog.DeepClone(); changedSystem["systemSha256"] = "changed";
        rejected = false;
        try { NativeLocations.Validate(changedSystem, identity); }
        catch (InvalidDataException) { rejected = true; }
        Require(rejected, "landmark system fingerprints are enforced");
        entries[0]!["world"]![0] = 2;
        File.WriteAllText(cache, catalog.ToJsonString());
        rejected = false;
        try { NativeLocations.Load(contextPath); } catch (InvalidDataException) { rejected = true; }
        Require(rejected && Projects.Read(cache)["entries"]![0]!["world"]![0]!.GetValue<int>() == 2,
            "corrupt cached locations are reported and preserved for diagnosis");
        File.Delete(cache);
        rejected = false;
        var missing = Path.Combine(root, "missing.json");
        try { NativeLocations.Export(contextPath, missing); } catch (IOException) { rejected = true; }
        Require(rejected && !File.Exists(cache) && !File.Exists(missing) &&
            File.ReadAllText(contextPath) == manifest,
            "missing resources cannot publish partial navigation or alter the world");
    }
}
