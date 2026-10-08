// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace CrystalBridge;

internal sealed record NativeLandmark(int ID, string Name, byte Biome, int[] World, string Kind = NativeLocations.HomePointKind);

internal static class NativeLocations
{
    internal const string FileName = "locations.json";
    internal const string ReviewedBiomes = "f79487f55e45182841e976785ea2a0094950917024b68b5b88b3d8d56470f75d";
    internal const string ReviewedSystem = "aeb36700a47795c8bda8ba3cbdb0ab9b131f4b56b1136f3dd5decf5ebb59178f";
    internal const string HomePointKind = "homePoint";
    internal const string TeleportPointKind = "teleportPoint";
    private const int Format = 2;
    private const int DynamicHomePointIndex = 1;
    internal const int MaxEntries = 256;
    private const int MaxBytes = 256 * 1024;
    private const int MaxText = 256;
    private static readonly Regex ZoneName = new(@"@Z([0-9]+)\.Name", RegexOptions.CultureInvariant);

    internal static JsonObject Build(IEnumerable<NativeLandmark> points, JsonElement[] biomes, string identity)
    {
        var names = biomes.Where(b => b.ValueKind == JsonValueKind.Object)
            .ToDictionary(b => b.GetProperty("ID").GetInt32(), b => b.GetProperty("Name").GetString()!);
        var entries = new JsonArray();
        foreach (var point in points.OrderBy(p => p.Kind, StringComparer.Ordinal).ThenBy(p => p.ID))
        {
            if (!names.TryGetValue(point.Biome, out var area))
                throw new InvalidDataException("Native location has an unknown biome");
            // Zone-name variables are native biome references; unsupported text remains identifiable
            var name = ZoneName.Replace(point.Name, match =>
                int.TryParse(match.Groups[1].Value, out var id) && names.TryGetValue(id, out var zone)
                    ? zone : match.Value);
            var resolved = !name.Contains('@');
            entries.Add(new JsonObject
            {
                ["id"] = point.ID, ["kind"] = point.Kind,
                ["name"] = resolved ? name : (point.Kind == HomePointKind ? $"Home point {point.ID}" : $"Landmark {point.ID}"),
                ["area"] = area, ["world"] = JsonSerializer.SerializeToNode(point.World),
                ["sourceName"] = point.Name, ["nameResolved"] = resolved
            });
            if (entries.Count > MaxEntries) throw new InvalidDataException("Native location catalog exceeds its limit");
        }
        var catalog = new JsonObject
        {
            ["format"] = Format, ["identity"] = identity, ["biomeSha256"] = ReviewedBiomes,
            ["systemSha256"] = ReviewedSystem, ["hasLandmarks"] = true,
            ["entriesSha256"] = Digest(entries), ["entries"] = entries
        };
        Validate(catalog, identity);
        return catalog;
    }

    private static string Digest(JsonArray entries) => NativeGame.Hash(System.Text.Encoding.UTF8.GetBytes(entries.ToJsonString()));

    internal static void Validate(JsonObject catalog, string identity, bool legacy = false)
    {
        if (JsonSerializer.SerializeToUtf8Bytes(catalog, Context.JsonOptions).Length > MaxBytes)
            throw new InvalidDataException("Native location catalog exceeds its size limit");
        if (catalog["format"]?.GetValue<int>() != (legacy ? 1 : Format) ||
            catalog["identity"]?.GetValue<string>() != identity ||
            catalog["biomeSha256"]?.GetValue<string>() != ReviewedBiomes ||
            catalog["entries"] is not JsonArray entries || entries.Count is < 1 or > MaxEntries ||
            catalog["entriesSha256"]?.GetValue<string>() != Digest(entries))
            throw new InvalidDataException("Native location catalog fingerprint mismatch");
        if (!legacy && (catalog["hasLandmarks"] is not JsonValue available || !available.TryGetValue<bool>(out var complete) ||
            (complete ? catalog["systemSha256"]?.GetValue<string>() != ReviewedSystem : catalog["systemSha256"] is not null)))
            throw new InvalidDataException("Native landmark source fingerprint mismatch");
        var ids = new HashSet<string>();
        foreach (var entry in entries)
        {
            if (entry is not JsonObject point) throw new InvalidDataException("Invalid native location entry");
            var kind = legacy ? HomePointKind : point["kind"]?.GetValue<string>();
            var id = point["id"]?.GetValue<int>() ?? -1;
            if (kind is not (HomePointKind or TeleportPointKind) ||
                (kind == HomePointKind ? id is < 1 or > NativeGame.LastVanillaEntity : id is < 0 or >= MaxEntries) ||
                !ids.Add(kind + ":" + id) ||
                point["world"] is not JsonArray world || world.Count != 3 ||
                !TiledContext.Inside(world.Select(v => v!.GetValue<int>()).ToArray()) ||
                point["nameResolved"] is not JsonValue resolved || !resolved.TryGetValue<bool>(out _))
                throw new InvalidDataException("Invalid native location identity or coordinates");
            foreach (var key in new[] { "name", "area", "sourceName" })
            {
                var text = point[key]?.GetValue<string>();
                if (string.IsNullOrWhiteSpace(text) || text.Length > MaxText || text.Any(char.IsControl))
                    throw new InvalidDataException("Invalid native location label");
            }
        }
    }

    internal static JsonObject Create(NativeGame game, NativeWorld world, JsonObject context, string directory, bool replaceLegacy = false)
    {
        if (world.WorldHash != NativeGame.ReviewedWorld)
            throw new InvalidDataException("Native location world differs from the inspected installation");
        var path = Path.Combine(game.Installation, "Content", "Database", "biome.dat");
        if (NativeGame.Hash(File.ReadAllBytes(path)) != ReviewedBiomes)
            throw new InvalidDataException("Native location biome definitions differ from the inspected installation");
        var systemPath = Path.Combine(game.Installation, "Content", "Database", "system.dat");
        if (NativeGame.Hash(File.ReadAllBytes(systemPath)) != ReviewedSystem)
            throw new InvalidDataException("Native landmark system definitions differ from the inspected installation");
        using var system = NativeGame.ReadDatabaseJson(systemPath);
        var landmarks = TeleportPoints(system.RootElement, point => world.Biome(point[0], point[1], point[2]));
        var catalog = Build(world.EntityChunks().SelectMany(game.HomePoints).Concat(landmarks),
            NativeGame.ReadDatabase(path), TiledContext.Identity(context));
        var output = Path.Combine(directory, FileName);
        var staging = output + "." + Guid.NewGuid().ToString("N") + ".partial";
        try
        {
            File.WriteAllText(staging, catalog.ToJsonString(Context.JsonOptions));
            File.Move(staging, output, replaceLegacy);
        }
        finally { if (File.Exists(staging)) File.Delete(staging); }
        return catalog;
    }

    internal static IEnumerable<NativeLandmark> TeleportPoints(JsonElement system, Func<int[], byte> biome)
    {
        var points = system.GetProperty("TeleportPoints");
        if (points.GetArrayLength() > MaxEntries) throw new InvalidDataException("Native landmark definitions exceed their limit");
        var index = -1;
        foreach (var point in points.EnumerateArray())
        {
            index++;
            // The Home Point slot is redirected to save state; its default coordinate is not a fixed landmark
            if (point.ValueKind == JsonValueKind.Null || index == DynamicHomePointIndex) continue;
            var coord = point.GetProperty("Coord");
            var world = new[] { "X", "Y", "Z" }.Select(axis => coord.GetProperty(axis).GetInt32()).ToArray();
            if (!TiledContext.Inside(world)) throw new InvalidDataException("Native landmark exceeds world bounds");
            yield return new NativeLandmark(index, point.GetProperty("Name").GetString()!, biome(world), world, TeleportPointKind);
        }
    }

    internal static JsonObject Load(string contextPath)
    {
        var context = Projects.Read(contextPath);
        TiledContext.Validate(contextPath, context);
        var directory = Path.GetDirectoryName(Path.GetFullPath(contextPath))!;
        var path = Path.Combine(directory, FileName);
        if (File.Exists(path))
        {
            if (new FileInfo(path).Length > MaxBytes) throw new InvalidDataException("Native location catalog exceeds its size limit");
            var catalog = Projects.Read(path);
            if (catalog["format"]?.GetValue<int>() == 1)
            {
                Validate(catalog, TiledContext.Identity(context), true);
                try
                {
                    using var migrationGame = new NativeGame(context["installation"]!.GetValue<string>());
                    var migrationWorld = new NativeWorld(Path.Combine(migrationGame.Installation, "Content", "Worlds", "field.dat"));
                    return Create(migrationGame, migrationWorld, context, directory, true);
                }
                catch (Exception error) when (error is IOException or InvalidDataException)
                {
                    // Keep a validated home-point catalog usable offline until its extra sources can be read
                    foreach (var entry in catalog["entries"]!.AsArray()) entry!["kind"] = HomePointKind;
                    catalog["format"] = Format;
                    catalog["hasLandmarks"] = false;
                    catalog["systemSha256"] = null;
                    catalog["warning"] = "Extra landmarks need the supported installation: " + error.Message;
                    catalog["entriesSha256"] = Digest(catalog["entries"]!.AsArray());
                    Validate(catalog, TiledContext.Identity(context));
                    return catalog;
                }
            }
            Validate(catalog, TiledContext.Identity(context));
            return catalog;
        }
        // A sidecar keeps existing world manifests and saved document fingerprints stable
        using var game = new NativeGame(context["installation"]!.GetValue<string>());
        var world = new NativeWorld(Path.Combine(game.Installation, "Content", "Worlds", "field.dat"));
        return Create(game, world, context, directory);
    }

    internal static void Export(string context, string output)
    {
        var catalog = Load(context);
        using var stream = new FileStream(output, FileMode.CreateNew);
        JsonSerializer.Serialize(stream, catalog, Context.JsonOptions);
        Console.WriteLine(JsonSerializer.Serialize(new { locations = catalog["entries"]!.AsArray().Count,
            hasLandmarks = catalog["hasLandmarks"]!.GetValue<bool>(), output }));
    }
}
