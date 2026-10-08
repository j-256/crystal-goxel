// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CrystalBridge;

internal sealed class TiledContext : IDisposable
{
    internal const int Format = 2;
    internal const int TileSize = 16;
    internal const int MaxViewTiles = 125;
    internal const int ViewTileRadius = 2;
    internal const int WorldExtent = 10000;
    internal static readonly int[] DocumentOrigin = [0, 0, 0];
    internal static readonly string[] SharedFiles = ["atlas.png", "palette.mesh"];
    internal static readonly string[] TileFiles = ["reference.mesh", "reference.cells", "biomes.bin"];
    private readonly string directory;
    private readonly JsonObject manifest;
    private NativeGame? game;
    private NativeWorld? world;
    private readonly Dictionary<string, byte[]> biomeCache = [];

    internal TiledContext(string path, JsonObject context)
    {
        Validate(path, context);
        directory = Path.GetDirectoryName(Path.GetFullPath(path))!;
        manifest = context;
    }

    internal static bool IsTiled(JsonObject context) => context["format"]?.GetValue<int>() == Format;
    internal static int TileIndex(int coordinate) => (int)Math.Floor((double)coordinate / TileSize);
    internal static int[] Key(int[] point) => point.Select(TileIndex).ToArray();
    internal static bool Inside(int[] point) => point.Length == 3 &&
        Math.Abs((long)point[0]) <= WorldExtent && point[1] >= 0 && point[1] < 256 &&
        Math.Abs((long)point[2]) <= WorldExtent;

    internal static List<int[]> Keys(int[] min, int[] max)
    {
        if (min.Length != 3 || max.Length != 3 || !Inside(min) ||
            !Inside(max.Select(v => v - 1).ToArray()) ||
            Enumerable.Range(0, 3).Any(i => min[i] >= max[i]))
            throw new InvalidDataException("Tile request exceeds supported world bounds");
        var first = Key(min); var last = Key(max.Select(v => v - 1).ToArray());
        if (Enumerable.Range(0, 3).Aggregate(1L, (n, i) => n * (last[i] - first[i] + 1)) > MaxViewTiles)
            throw new InvalidDataException("Editing area exceeds the tile preparation limit; use a smaller selection");
        var result = new List<int[]>();
        for (var x = first[0]; x <= last[0]; x++)
            for (var y = first[1]; y <= last[1]; y++)
                for (var z = first[2]; z <= last[2]; z++) result.Add([x, y, z]);
        return result;
    }

    internal static (int[] Min, int[] Max) ViewBounds(int[] center)
    {
        if (!Inside(center)) throw new ArgumentException("Location exceeds supported world bounds");
        var key = Key(center);
        var min = key.Select(k => (k - ViewTileRadius) * TileSize).ToArray();
        var max = key.Select(k => (k + ViewTileRadius + 1) * TileSize).ToArray();
        min[0] = Math.Max(min[0], -WorldExtent); max[0] = Math.Min(max[0], WorldExtent + 1);
        min[1] = Math.Max(min[1], 0); max[1] = Math.Min(max[1], 256);
        min[2] = Math.Max(min[2], -WorldExtent); max[2] = Math.Min(max[2], WorldExtent + 1);
        return (min, max);
    }

    internal static string Identity(JsonObject context) => NativeGame.Hash(
        System.Text.Encoding.UTF8.GetBytes(string.Join('|',
            context["executableSha256"]!.GetValue<string>(),
            context["worldSha256"]!.GetValue<string>(),
            context["voxelSha256"]!.GetValue<string>(),
            context["files"]!["atlas.png"]!.GetValue<string>(),
            context["files"]!["palette.mesh"]!.GetValue<string>())));

    internal static void Validate(string path, JsonObject context)
    {
        if (!IsTiled(context) || context["tileSize"]?.GetValue<int>() != TileSize ||
            context["executableSha256"]?.GetValue<string>() != NativeGame.ReviewedExecutable ||
            context["worldSha256"]?.GetValue<string>() != NativeGame.ReviewedWorld ||
            context["voxelSha256"]?.GetValue<string>() != NativeGame.ReviewedVoxels ||
            context["origin"]?.ToJsonString() != "[0,0,0]" ||
            string.IsNullOrWhiteSpace(context["installation"]?.GetValue<string>()))
            throw new InvalidDataException("Unsupported tiled world context");
        CheckFiles(Path.GetDirectoryName(Path.GetFullPath(path))!, context, SharedFiles);
    }

    internal static void CheckFiles(string directory, JsonObject context, IEnumerable<string> names)
    {
        foreach (var name in names)
        {
            var path = Path.Combine(directory, name);
            if (new FileInfo(path).Length > 256 * 1024 * 1024 ||
                NativeGame.Hash(File.ReadAllBytes(path)) != context["files"]?[name]?.GetValue<string>())
                throw new InvalidDataException("Context fingerprint mismatch: " + name);
        }
    }

    internal static void Create(string installation, string output)
    {
        if (Directory.Exists(output) || File.Exists(output))
            throw new ArgumentException("Output already exists; choose a new world cache directory");
        using var game = new NativeGame(installation);
        var world = new NativeWorld(Path.Combine(game.Installation, "Content", "Worlds", "field.dat"));
        if (world.WorldHash != NativeGame.ReviewedWorld)
            throw new InvalidDataException("Native world differs from the inspected installation");
        var staging = Path.GetFullPath(output) + "." + Guid.NewGuid().ToString("N") + ".partial";
        Directory.CreateDirectory(staging);
        try
        {
            var blocks = Context.WriteAssets(game, staging);
            var files = SharedFiles.ToDictionary(n => n, n => NativeGame.Hash(File.ReadAllBytes(Path.Combine(staging, n))));
            var context = new
            {
                format = Format, tileSize = TileSize, origin = DocumentOrigin,
                gameVersion = "Windows 1.6.9.0", installation = game.Installation,
                executableSha256 = game.ExecutableHash, worldSha256 = world.WorldHash,
                voxelSha256 = NativeGame.ReviewedVoxels, blocks, files
            };
            File.WriteAllText(Path.Combine(staging, "world.json"), JsonSerializer.Serialize(context, Context.JsonOptions));
            Directory.Move(staging, Path.GetFullPath(output));
        }
        catch { Directory.Delete(staging, true); throw; }
        Console.WriteLine(JsonSerializer.Serialize(new { context = Path.Combine(Path.GetFullPath(output), "world.json") }));
    }

    private string TileDirectory(int[] key) => Path.Combine(directory, "tiles", string.Join(',', key));

    internal JsonObject ReadTile(int[] key)
    {
        var root = TileDirectory(key);
        var tile = Projects.Read(Path.Combine(root, "tile.json"));
        if (tile["format"]?.GetValue<int>() != 1 ||
            tile["identity"]?.GetValue<string>() != Identity(manifest) ||
            tile["origin"]?.ToJsonString() != JsonSerializer.Serialize(key.Select(v => v * TileSize)) ||
            tile["size"]?.ToJsonString() != "[16,16,16]")
            throw new InvalidDataException("Tile identity or owned bounds differ from the world context");
        CheckFiles(root, tile, TileFiles);
        if (new FileInfo(Path.Combine(root, "biomes.bin")).Length != TileSize * TileSize * TileSize)
            throw new InvalidDataException("Invalid tile biome length");
        tile["path"] = root;
        return tile;
    }

    internal JsonObject Ensure(int[] key)
    {
        var destination = TileDirectory(key);
        // Existing corrupt tiles are errors, never silently regenerated from a different installation
        if (Directory.Exists(destination)) return ReadTile(key);
        if (game is null)
        {
            game = new NativeGame(manifest["installation"]!.GetValue<string>());
            world = new NativeWorld(Path.Combine(game.Installation, "Content", "Worlds", "field.dat"));
            if (world.WorldHash != NativeGame.ReviewedWorld ||
                NativeGame.Hash(Context.Atlas(Path.Combine(game.Installation, "Content", "Textures", "Voxel.dat"))) != manifest["files"]!["atlas.png"]!.GetValue<string>())
                throw new InvalidDataException("Installed native resources differ from the world context");
        }
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        var staging = destination + "." + Guid.NewGuid().ToString("N") + ".partial";
        Directory.CreateDirectory(staging);
        try
        {
            var origin = key.Select(v => v * TileSize).ToArray();
            // Only owned cells emit geometry; the native builder can sample across every tile edge
            var visible = Context.WriteRegion(game.Visible, game.Build, world!, origin,
                [TileSize, TileSize, TileSize], staging);
            var files = TileFiles.ToDictionary(n => n, n => NativeGame.Hash(File.ReadAllBytes(Path.Combine(staging, n))));
            File.WriteAllText(Path.Combine(staging, "tile.json"), JsonSerializer.Serialize(new
            {
                format = 1, identity = Identity(manifest), origin,
                size = new[] { TileSize, TileSize, TileSize }, visible, files
            }, Context.JsonOptions));
            try { Directory.Move(staging, destination); }
            catch (IOException) when (Directory.Exists(destination)) { Directory.Delete(staging, true); }
        }
        catch { if (Directory.Exists(staging)) Directory.Delete(staging, true); throw; }
        return ReadTile(key);
    }

    internal void Prepare(int[] min, int[] max, string output)
    {
        var tiles = new JsonArray();
        foreach (var key in Keys(min, max)) tiles.Add(Ensure(key));
        // A request is published only when every tile is ready; an interrupted request leaves reusable cache files
        using var file = new FileStream(output, FileMode.CreateNew, FileAccess.Write);
        JsonSerializer.Serialize(file, new JsonObject
        {
            ["format"] = 1, ["identity"] = Identity(manifest), ["tiles"] = tiles,
            ["min"] = JsonSerializer.SerializeToNode(min), ["max"] = JsonSerializer.SerializeToNode(max)
        }, Context.JsonOptions);
        Console.WriteLine(JsonSerializer.Serialize(new { tiles = tiles.Count, view = Path.GetFullPath(output) }));
    }

    internal byte Biome(int[] point)
    {
        if (!Inside(point)) throw new InvalidDataException("Authored cell exceeds supported world bounds");
        var key = Key(point);
        var name = string.Join(',', key);
        if (!biomeCache.TryGetValue(name, out var bytes))
        {
            Ensure(key);
            bytes = File.ReadAllBytes(Path.Combine(TileDirectory(key), "biomes.bin"));
            biomeCache[name] = bytes;
        }
        var local = point.Zip(key, (v, k) => v - k * TileSize).ToArray();
        return bytes[(local[0] * TileSize + local[1]) * TileSize + local[2]];
    }

    public void Dispose() => game?.Dispose();
}
