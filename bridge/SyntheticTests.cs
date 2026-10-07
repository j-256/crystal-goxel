// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Compression;
using System.Text.Json.Nodes;

namespace CrystalBridge;

internal static class SyntheticTests
{
    internal static void Run()
    {
        var scratch = Path.Combine(Path.GetTempPath(), "crystal-goxel-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(scratch);
        try { ProjectsRoundTrip(scratch); WorldArchive(scratch); }
        finally { Directory.Delete(scratch, true); }
    }

    private static void Require(bool condition, string label)
    {
        if (!condition) throw new InvalidDataException("Test failed: " + label);
    }

    private static void ProjectsRoundTrip(string directory)
    {
        var context = new JsonObject
        {
            ["format"] = 1,
            ["executableSha256"] = NativeGame.ReviewedExecutable,
            ["worldSha256"] = NativeGame.ReviewedWorld,
            ["voxelSha256"] = NativeGame.ReviewedVoxels,
            ["origin"] = new JsonArray(-4, 10, -4),
            ["size"] = new JsonArray(8, 8, 8),
            ["blocks"] = new JsonArray(new JsonObject { ["id"] = 1, ["maxVariant"] = 1, ["collides"] = true }, new JsonObject { ["id"] = 2, ["maxVariant"] = 3, ["collides"] = false }),
            ["files"] = new JsonObject()
        };
        foreach (var name in new[] { "reference.mesh", "reference.cells", "palette.mesh", "atlas.png", "biomes.bin" })
        {
            // Project tests need hashed synthetic resources, never a game installation or native art
            var bytes = name == "biomes.bin" ? Enumerable.Repeat((byte)5, 512).ToArray() : "synthetic resource"u8.ToArray();
            File.WriteAllBytes(Path.Combine(directory, name), bytes);
            context["files"]![name] = NativeGame.Hash(bytes);
        }
        var manifest = Path.Combine(directory, "context.json"); File.WriteAllText(manifest, context.ToJsonString());
        var source = Projects.ReadJsonForTests("""
{"ID":"synthetic-project","Title":"Fixture","EditorVersion":34,"SteamWorkshopFileID":18446744073709551614,"Future":{"quantity":9007199254740993},"Entities":[{"ID":5000,"EntityType":0,"Coord":{"X":-1,"Y":12,"Z":-1},"BiomeID":5,"FutureEntity":"preserve","NpcData":{"Key":"Static","Outfits":[{"Condition":{"ConditionType":0,"Data":null,"IsNegation":false},"VoxelID":1,"VoxelVariantIndex":0,"WanderType":0,"FutureOutfit":123}],"Pages":[]}},{"ID":6000,"EntityType":0,"Coord":{"X":0,"Y":12,"Z":0},"NpcData":{"Outfits":[{"VoxelID":1,"WanderType":1}],"Pages":[]}},{"ID":3824,"EntityType":5,"FutureTreasure":"untouched"}],"Tree":[{"ModelTypeID":1,"ModelID":1,"FutureFolder":"keep","Children":[{"ModelTypeID":18,"ModelID":5000,"FutureTree":true},{"ModelTypeID":18,"ModelID":7000}]}]}
""");
        var importedOutfit = source["Entities"]![0]!["NpcData"]!["Outfits"]![0]!;
        importedOutfit["MountType"] = 16;
        importedOutfit["PlayerCollision"] = 1;
        importedOutfit["NpcCollision"] = 2;
        var input = Path.Combine(directory, "source.json"); File.WriteAllText(input, source.ToJsonString());
        var snapshotPath = Path.Combine(directory, "snapshot.json"); Projects.Import(manifest, input, snapshotPath);
        var snapshot = Projects.Read(snapshotPath);
        Require(snapshot["cells"]!.AsArray().Count == 1, "only static supported NPC is authored");
        Require(snapshot["cells"]![0]!["pos"]!.ToJsonString() == "[3,-4,2]", "NPC coordinate conversion");
        snapshot["cells"]![0]!["type"] = 2; snapshot["cells"]![0]!["variant"] = 3;
        snapshot["cells"]!.AsArray().Add(new JsonObject { ["pos"] = new JsonArray(4, -4, 2), ["type"] = 2, ["variant"] = 0 });
        File.WriteAllText(snapshotPath, snapshot.ToJsonString());
        var export = Path.Combine(directory, "export.json"); Projects.Export(manifest, snapshotPath, export);
        var result = Projects.Read(export);
        Require(result["SteamWorkshopFileID"]!.ToJsonString() == "18446744073709551614" && result["Future"]!["quantity"]!.ToJsonString() == "9007199254740993", "large numeric fields survive full export");
        var entities = result["Entities"]!.AsArray();
        foreach (var id in new[] { 3824, 6000 })
            Require(JsonNode.DeepEquals(entities.Single(e => e!["ID"]!.GetValue<int>() == id), source["Entities"]!.AsArray().Single(e => e!["ID"]!.GetValue<int>() == id)), "unrelated entity preserved");
        var original = entities.Single(e => e!["ID"]!.GetValue<int>() == 5000)!;
        Require(original["FutureEntity"]!.GetValue<string>() == "preserve" && original["NpcData"]!["Outfits"]![0]!["FutureOutfit"]!.GetValue<int>() == 123, "managed unknown fields preserved");
        Require(original["NpcData"]!["Outfits"]![0]!["VoxelVariantIndex"]!.GetValue<int>() == 3, "changed native variant retained");
        foreach (var key in new[] { "MountType", "PlayerCollision", "NpcCollision" })
            Require(JsonNode.DeepEquals(original["NpcData"]!["Outfits"]![0]![key], importedOutfit[key]), "imported physics settings preserved: " + key);
        var created = entities.Single(e => e!["ID"]!.GetValue<int>() == 7001)!["NpcData"]!["Outfits"]![0]!;
        Require(created["MountType"]!.GetValue<int>() == 20 && created["WanderType"]!.GetValue<int>() == 0, "new construction uses fixed zero-gravity NPC physics");
        Require(created["PlayerCollision"]!.GetValue<int>() == 3 && created["NpcCollision"]!.GetValue<int>() == 3, "new construction is solid even for a decorative native block");
        snapshot["newObjectsSolid"] = false;
        File.WriteAllText(snapshotPath, snapshot.ToJsonString());
        var decorative = Path.Combine(directory, "decorative.json");
        Projects.Export(manifest, snapshotPath, decorative);
        var decoration = Projects.Read(decorative)["Entities"]!.AsArray().Single(e => e!["ID"]!.GetValue<int>() == 7001)!["NpcData"]!["Outfits"]![0]!;
        Require(decoration["MountType"]!.GetValue<int>() == 20 && decoration["PlayerCollision"]!.GetValue<int>() == 0 && decoration["NpcCollision"]!.GetValue<int>() == 0, "non-solid decoration remains fixed in place");
        snapshot.Remove("newObjectsSolid");
        File.WriteAllText(snapshotPath, snapshot.ToJsonString());
        Require(entities.Single(e => e!["ID"]!.GetValue<int>() == 7001)!["BiomeID"]!.GetValue<int>() == 5, "collision-free ID accounts for nested tree references and native biome");
        Require(result["Tree"]![0]!["Children"]![0]!["FutureTree"]!.GetValue<bool>(), "nested editor tree metadata retained");
        PreservationCases(directory, manifest, source, snapshot);
        var protectedOutput = false;
        try { Projects.Export(manifest, snapshotPath, export); } catch (IOException) { protectedOutput = true; }
        Require(protectedOutput, "existing project never overwritten");
        snapshot["cells"]![0]!["variant"] = 4;
        File.WriteAllText(snapshotPath, snapshot.ToJsonString());
        var invalid = Path.Combine(directory, "invalid.json"); var rejected = false;
        try { Projects.Export(manifest, snapshotPath, invalid); } catch (InvalidDataException) { rejected = true; }
        Require(rejected && !File.Exists(invalid), "invalid identity rejected before writing output");
        File.AppendAllText(Path.Combine(directory, "palette.mesh"), "tamper"); rejected = false;
        try { Projects.ValidateContext(manifest, context); } catch (InvalidDataException) { rejected = true; }
        Require(rejected, "context asset fingerprint mismatch rejected");
    }

    private static void PreservationCases(string directory, string manifest, JsonObject source, JsonObject snapshot)
    {
        var snapshotPath = Path.Combine(directory, "case-snapshot.json");
        var deleted = (JsonObject)snapshot.DeepClone();
        deleted["cells"]!.AsArray().RemoveAt(0);
        File.WriteAllText(snapshotPath, deleted.ToJsonString());
        var output = Path.Combine(directory, "deleted.json");
        Projects.Export(manifest, snapshotPath, output);
        var result = Projects.Read(output);
        Require(result["Entities"]!.AsArray().All(e => e!["ID"]!.GetValue<int>() != 5000), "deleted managed entity removed");
        Require(result["Tree"]![0]!["Children"]!.AsArray().Count == 1 && result["Tree"]![0]!["FutureFolder"]!.GetValue<string>() == "keep", "nested managed node pruned without losing folder metadata");

        var preserved = (JsonObject)source.DeepClone();
        var copy = (JsonObject)preserved["Entities"]![0]!.DeepClone();
        copy["ID"] = 5001;
        preserved["Entities"]!.AsArray().Add(copy);
        var conditional = (JsonObject)copy.DeepClone();
        conditional["ID"] = 6500; conditional["Coord"]!["X"] = 1;
        conditional["NpcData"]!["Outfits"]![0]!["Condition"]!["IsNegation"] = true;
        preserved["Entities"]!.AsArray().Add(conditional);
        var input = Path.Combine(directory, "preserved-source.json");
        File.WriteAllText(input, preserved.ToJsonString());
        Projects.Import(manifest, input, Path.Combine(directory, "preserved-snapshot.json"));
        var untouched = Projects.Read(Path.Combine(directory, "preserved-snapshot.json"));
        Require(untouched["cells"]!.AsArray().Count == 0 && untouched["managed"]!.AsArray().Count == 0, "both overlapping NPCs and a negated condition remain unmanaged");
        Projects.Export(manifest, Path.Combine(directory, "preserved-snapshot.json"), Path.Combine(directory, "preserved-output.json"));
        Require(JsonNode.DeepEquals(Projects.Read(Path.Combine(directory, "preserved-output.json"))["Entities"], preserved["Entities"]), "unsupported entities survive an empty authored export");

        void Reject(JsonObject invalid, string name)
        {
            File.WriteAllText(snapshotPath, invalid.ToJsonString());
            var destination = Path.Combine(directory, name + ".json");
            var rejected = false;
            try { Projects.Export(manifest, snapshotPath, destination); } catch (InvalidDataException) { rejected = true; }
            Require(rejected && !File.Exists(destination), name);
        }
        var mapping = (JsonObject)snapshot.DeepClone();
        mapping["managed"]![0]!["world"]![0] = 0;
        Reject(mapping, "managed world mapping must match original entity");
        var bounds = (JsonObject)snapshot.DeepClone(); bounds["cells"]![0]!["pos"]![0] = 99;
        Reject(bounds, "outside-context cell rejected");
        var duplicate = (JsonObject)snapshot.DeepClone();
        duplicate["cells"]!.AsArray().Add(duplicate["cells"]![0]!.DeepClone());
        Reject(duplicate, "duplicate authored cell rejected");
        var option = (JsonObject)snapshot.DeepClone(); option["newObjectsSolid"] = "true";
        Reject(option, "collision option must be boolean");
        option["newObjectsSolid"] = null;
        Reject(option, "collision option must not be null");
        var exhausted = (JsonObject)snapshot.DeepClone(); var maximum = (JsonObject)source.DeepClone();
        maximum["Entities"]!.AsArray().Add(new JsonObject { ["ID"] = int.MaxValue, ["EntityType"] = 5 });
        exhausted["source"] = maximum.ToJsonString();
        Reject(exhausted, "entity ID exhaustion rejected");
    }

    private static void WorldArchive(string directory)
    {
        byte[] Zip(string name, byte[] bytes)
        {
            using var stream = new MemoryStream();
            using (var archive = new ZipArchive(stream, ZipArchiveMode.Create, true))
            using (var entry = archive.CreateEntry(name).Open()) entry.Write(bytes);
            return stream.ToArray();
        }
        var chunk = new byte[16_397]; chunk[0] = 7;
        Array.Fill(chunk, (byte)5, 5, 8);
        var last = 13 + 4095 * 4; chunk[last] = 1; chunk[last + 1] = 64; chunk[last + 2] = 15;
        var region = Zip("y0.dat", chunk);
        using var field = new MemoryStream();
        using (var writer = new BinaryWriter(field, System.Text.Encoding.UTF8, true))
        {
            writer.Write(0); writer.Write(0); writer.Write(1);
            writer.Write(-1); writer.Write(-1); writer.Write(0); writer.Write(region.Length);
            writer.Write(0); writer.Write(region.Length); writer.Write(region);
        }
        var path = Path.Combine(directory, "field.dat");
        File.WriteAllBytes(path, new byte[] { 5, 0 }.Concat(Zip("field.dat", field.ToArray())).ToArray());
        var world = new NativeWorld(path);
        Require(world.Get(-1, 15, -1) == new Cell(1, 64, 15), "negative native region boundary and cell order");
        Require(world.Biome(-1, 15, -1) == 5, "native biome octant indexing");
        Require(world.Get(0, 15, 0).Type == 0 && world.Get(-1, -1, -1) == Cell.Air, "missing and vertical boundary cells");
        File.WriteAllBytes(path, [0, 0]); var rejected = false;
        try { _ = new NativeWorld(path); } catch (InvalidDataException) { rejected = true; }
        Require(rejected, "unsupported native archive rejected");
    }
}
