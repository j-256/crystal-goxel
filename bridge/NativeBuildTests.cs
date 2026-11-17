// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CrystalBridge;

internal static class NativeBuildTests
{
    private static void Require(bool condition, string label)
    {
        if (!condition) throw new InvalidDataException("Native build test failed: " + label);
    }

    private static void Reject(Action action, string label)
    {
        var rejected = false;
        try { action(); }
        catch (InvalidDataException) { rejected = true; }
        Require(rejected, label);
    }

    internal static void Run(string directory)
    {
        var profiles = NativeBuilds.Profiles;
        foreach (var profile in profiles)
        {
            Require(NativeBuilds.FromExecutable(profile.ExecutableHash) == profile,
                "verified executables resolve to their source platform and version");
            var root = Path.Combine(directory, profile.Platform + profile.Version);
            var context = TileTests.Fixture(root);
            context["executableSha256"] = profile.ExecutableHash;
            context["worldSha256"] = profile.WorldHash;
            context["voxelSha256"] = new string('0', NativeBuilds.FingerprintLength);
            Reject(() => NativeBuilds.FromContext(context), "voxel fingerprints remain bound to the source build");
            context["voxelSha256"] = profile.VoxelHash;
            context["gameVersion"] = profile.Label;
            Require(NativeBuilds.FromContext(context) == profile, "context source identity retains its verified build");
            TiledContext.Validate(Path.Combine(root, "world.json"), context);
            NativeBuilds.RequireSame(context, profile);
            var other = profiles.FirstOrDefault(p => p.Version == profile.Version && p.Platform != profile.Platform) ??
                profiles.First(p => p != profile);
            Reject(() => NativeBuilds.RequireSame(context, other), "uncached terrain cannot use another source build");
            context["worldSha256"] = other.WorldHash;
            Reject(() => NativeBuilds.FromContext(context), "executable and world fingerprints cannot be mixed");
            context["worldSha256"] = profile.WorldHash;
            context["gameVersion"] = "unverified label";
            Reject(() => NativeBuilds.FromContext(context), "version metadata cannot contradict source identity");
            context.Remove("gameVersion");
            Require(NativeBuilds.FromContext(context) == profile, "older manifests without labels remain readable");
            var biomes = JsonSerializer.Deserialize<JsonElement[]>("[{\"ID\":1,\"Name\":\"Synthetic area\"}]")!;
            var identity = TiledContext.Identity(context);
            var catalog = NativeLocations.Build([new(1, "Synthetic home", 1, [0, 99, 0])], biomes, identity, profile);
            NativeLocations.Validate(catalog, identity, profile);
            File.WriteAllText(Path.Combine(root, "world.json"), context.ToJsonString());
            File.WriteAllText(Path.Combine(root, NativeLocations.FileName), catalog.ToJsonString());
            Require(JsonNode.DeepEquals(NativeLocations.Load(Path.Combine(root, "world.json")), catalog),
                "all verified source profiles retain offline cached destinations");
            Reject(() => NativeLocations.Validate(catalog, "another context", profile),
                "location catalogs remain bound to their source context");
        }
        Reject(() => NativeBuilds.FromExecutable(new string('0', NativeBuilds.FingerprintLength)), "unknown executables remain explicit unsupported state");

        var catalogData = new NativeBuildCatalog(NativeBuilds.LatestVersion, profiles.ToArray());
        NativeBuildCatalog Read(NativeBuildCatalog value)
        {
            using var stream = new MemoryStream(JsonSerializer.SerializeToUtf8Bytes(value));
            return NativeBuilds.ReadCatalog(stream);
        }
        Require(Read(catalogData).Profiles.SequenceEqual(profiles), "compatibility profiles load as data without version-specific code");
        Reject(() => Read(catalogData with { LatestVersion = "99.0.0.0" }), "latest version must have a verified source profile");
        Reject(() => Read(catalogData with { Profiles = profiles.Append(profiles[0]).ToArray() }), "duplicate source identities are rejected");
        Reject(() => Read(catalogData with { Profiles = [profiles[0] with { VoxelHash = "invalid" }], LatestVersion = profiles[0].Version }),
            "malformed source fingerprints are rejected");

        var install = Path.Combine(directory, "synthetic installation");
        Directory.CreateDirectory(install);
        File.WriteAllText(Path.Combine(install, "Crystal Project.exe"), "synthetic executable");
        Require(NativeBuilds.ResourceRoot(install) == Path.GetFullPath(install), "Windows folders resolve directly");
        var app = Path.Combine(directory, "Synthetic Game.app");
        var resources = Path.Combine(app, "Contents", "Resources");
        Directory.CreateDirectory(resources);
        File.WriteAllText(Path.Combine(resources, "Crystal Project.exe"), "synthetic executable");
        Require(NativeBuilds.ResourceRoot(app) == resources && NativeBuilds.ResourceRoot(resources) == resources,
            "Mac bundles and resource folders select the same managed resource root");
        var missing = false;
        try { NativeBuilds.ResourceRoot(Path.Combine(directory, "missing game")); }
        catch (DirectoryNotFoundException) { missing = true; }
        Require(missing, "missing installations fail before native hosting");
    }
}
