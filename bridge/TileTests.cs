// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Compression;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CrystalBridge;

internal static class TileTests
{
    private static void Require(bool condition, string label)
    {
        if (!condition) throw new InvalidDataException("Tile test failed: " + label);
    }

    internal static JsonObject Fixture(string directory)
    {
        Directory.CreateDirectory(directory);
        var context = new JsonObject
        {
            ["format"] = TiledContext.Format, ["tileSize"] = TiledContext.TileSize,
            ["origin"] = new JsonArray(0, 0, 0), ["installation"] = "missing-synthetic-installation",
            ["executableSha256"] = NativeGame.ReviewedExecutable,
            ["worldSha256"] = NativeGame.ReviewedWorld, ["voxelSha256"] = NativeGame.ReviewedVoxels,
            ["blocks"] = new JsonArray(new JsonObject { ["id"] = 1, ["maxVariant"] = 1 }),
            ["files"] = new JsonObject()
        };
        foreach (var name in TiledContext.SharedFiles)
        {
            var bytes = "synthetic shared resource"u8.ToArray();
            File.WriteAllBytes(Path.Combine(directory, name), bytes);
            context["files"]![name] = NativeGame.Hash(bytes);
        }
        File.WriteAllText(Path.Combine(directory, "world.json"), context.ToJsonString());
        return context;
    }

    internal static void FixtureTile(string directory, JsonObject context, int[] key, byte biome)
    {
        var root = Path.Combine(directory, "tiles", string.Join(',', key));
        Directory.CreateDirectory(root);
        var files = new JsonObject();
        foreach (var name in TiledContext.TileFiles)
        {
            var bytes = name == "biomes.bin" ? Enumerable.Repeat(biome, 4096).ToArray() : "synthetic tile resource"u8.ToArray();
            File.WriteAllBytes(Path.Combine(root, name), bytes);
            files[name] = NativeGame.Hash(bytes);
        }
        File.WriteAllText(Path.Combine(root, "tile.json"), new JsonObject
        {
            ["format"] = 1, ["identity"] = TiledContext.Identity(context),
            ["origin"] = JsonSerializer.SerializeToNode(key.Select(v => v * TiledContext.TileSize)),
            ["size"] = new JsonArray(16, 16, 16), ["files"] = files
        }.ToJsonString());
    }

    internal static void Run(string directory)
    {
        Require(TiledContext.TileIndex(-1) == -1 && TiledContext.TileIndex(-16) == -1 &&
            TiledContext.TileIndex(-17) == -2, "negative tile ownership uses floor division");
        Require(TiledContext.Keys([-1, 5, 0], [1, 6, 1]).Count == 2 &&
            TiledContext.Keys([-16, 0, 0], [0, 16, 16]).Count == 1,
            "adjacent tiles have disjoint ownership and exclusive upper bounds");
        var rejected = false;
        try { TiledContext.Keys([0, 0, 0], [200, 200, 200]); }
        catch (InvalidDataException) { rejected = true; }
        Require(rejected, "large requests are bounded before cache work");
        var root = Path.Combine(directory, "tiles-contract");
        var context = Fixture(root);
        FixtureTile(root, context, [-1, 0, 0], 3);
        FixtureTile(root, context, [0, 0, 0], 7);
        using var tiled = new TiledContext(Path.Combine(root, "world.json"), context);
        Require(tiled.Biome([-1, 5, 0]) == 3 && tiled.Biome([0, 5, 0]) == 7,
            "cached neighboring tiles work without a game installation");
        var output = Path.Combine(root, "view.json");
        tiled.Prepare([-1, 5, 0], [1, 6, 1], output);
        Require(Projects.Read(output)["tiles"]!.AsArray().Count == 2, "view publishes both validated tiles");
        var incomplete = Path.Combine(root, "incomplete.json");
        rejected = false;
        try { tiled.Prepare([-1, 5, 0], [17, 6, 1], incomplete); }
        catch (IOException) { rejected = true; }
        Require(rejected && !File.Exists(incomplete), "missing tile never publishes a partial view");
        File.AppendAllText(Path.Combine(root, "tiles", "-1,0,0", "reference.mesh"), "tamper");
        rejected = false;
        try { tiled.ReadTile([-1, 0, 0]); }
        catch (InvalidDataException) { rejected = true; }
        Require(rejected, "corrupt tile remains an explicit failure");
        SeamGeometry(directory);
    }

    private static void SeamGeometry(string directory)
    {
        byte[] Zip(string name, byte[] bytes)
        {
            using var stream = new MemoryStream();
            using (var archive = new ZipArchive(stream, ZipArchiveMode.Create, true))
            using (var entry = archive.CreateEntry(name).Open()) entry.Write(bytes);
            return stream.ToArray();
        }
        byte[] Region(int x, byte marker)
        {
            var chunk = new byte[16_397]; chunk[0] = 7;
            var index = 13 + (x * 256 + 5 * 16) * 4;
            chunk[index] = 1; chunk[index + 1] = marker;
            return Zip("y0.dat", chunk);
        }
        var left = Region(15, 1); var right = Region(0, 2);
        using var field = new MemoryStream();
        using (var writer = new BinaryWriter(field, System.Text.Encoding.UTF8, true))
        {
            writer.Write(0); writer.Write(0); writer.Write(2);
            writer.Write(-1); writer.Write(0); writer.Write(0); writer.Write(left.Length);
            writer.Write(0); writer.Write(0); writer.Write(left.Length); writer.Write(right.Length);
            writer.Write(0); writer.Write(left.Length + right.Length);
            writer.Write(left); writer.Write(right);
        }
        var path = Path.Combine(directory, "seam-world.dat");
        File.WriteAllBytes(path, new byte[] { 5, 0 }.Concat(Zip("field.dat", field.ToArray())).ToArray());
        var world = new NativeWorld(path);
        var samples = 0;
        NativeVertex[] Build(Cell cell, Func<int, int, int, Cell> neighbor)
        {
            Require(neighbor(cell.Parameters == 1 ? 1 : -1, 0, 0).Type == 1,
                "native mesh sampling crosses the owned tile edge");
            samples++;
            return [new(0, 0, 0, 0, 0, 255, 255, 255, 255, 4),
                new(1, 0, 0, 0, 0, 255, 255, 255, 255, 4),
                new(0, 0, 1, 0, 0, 255, 255, 255, 255, 4),
                new(1, 0, 1, 0, 0, 255, 255, 255, 255, 4)];
        }
        foreach (var x in new[] { -16, 0 })
        {
            var root = Path.Combine(directory, "mesh-" + x);
            Directory.CreateDirectory(root);
            Require(Context.WriteRegion(id => id == 1, Build, world, [x, 0, 0], [16, 16, 16], root) == 1,
                "each terrain cell is emitted by exactly one tile");
            using var reader = new BinaryReader(File.OpenRead(Path.Combine(root, "reference.cells")));
            reader.ReadInt32();
            Require(reader.ReadInt32() == 2, "triangle ownership matches the emitted quad");
            var owner = new[] { reader.ReadInt32(), reader.ReadInt32(), reader.ReadInt32() };
            Require(Context.ToWorld(owner, [x, 0, 0]).SequenceEqual(new[] { x == -16 ? -1 : 0, 5, 0 }),
                "tile-local ownership maps to exact world coordinates");
        }
        Require(samples == 2, "neighbor sampling does not duplicate owned geometry");
    }
}
