// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CrystalBridge;

internal sealed record NativeBuild(string Version, string Platform, string ExecutableHash,
    string WorldHash, string VoxelHash, string EditorHash, string BiomeHash, string SystemHash)
{
    internal string Label => Platform + " " + Version;
}

internal sealed record NativeBuildCatalog(string LatestVersion, NativeBuild[] Profiles);

internal static class NativeBuilds
{
    internal const int FingerprintLength = 64;
    private const string CatalogResource = "CrystalBridge.NativeBuilds.json";
    private const string ExecutableName = "Crystal Project.exe";
    private static readonly NativeBuildCatalog Catalog = LoadCatalog();
    internal static string LatestVersion => Catalog.LatestVersion;
    internal static IReadOnlyList<NativeBuild> Profiles { get; } = Array.AsReadOnly(Catalog.Profiles);

    // The embedded table contains source identities, never native data
    // Keeping older rows preserves existing cache and document fingerprints
    private static NativeBuildCatalog LoadCatalog()
    {
        using var stream = typeof(NativeBuilds).Assembly.GetManifestResourceStream(CatalogResource) ??
            throw new InvalidDataException("Native compatibility table is missing from the helper");
        return ReadCatalog(stream);
    }

    internal static NativeBuildCatalog ReadCatalog(Stream stream)
    {
        var catalog = JsonSerializer.Deserialize<NativeBuildCatalog>(stream,
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        if (catalog?.Profiles is not { Length: > 0 } ||
            catalog.Profiles.Any(profile => profile is null) ||
            !System.Version.TryParse(catalog.LatestVersion, out _) ||
            !catalog.Profiles.Any(p => p.Version == catalog.LatestVersion) ||
            catalog.Profiles.GroupBy(p => p.ExecutableHash).Any(group => group.Count() != 1) ||
            catalog.Profiles.GroupBy(p => (p.Version, p.Platform)).Any(group => group.Count() != 1))
            throw new InvalidDataException("Invalid native compatibility table");
        foreach (var profile in catalog.Profiles)
        {
            var hashes = new[] { profile.ExecutableHash, profile.WorldHash, profile.VoxelHash,
                profile.EditorHash, profile.BiomeHash, profile.SystemHash };
            if (!System.Version.TryParse(profile.Version, out _) || string.IsNullOrWhiteSpace(profile.Platform) ||
                hashes.Any(hash => hash is null || hash.Length != FingerprintLength ||
                    hash.Any(c => c is not (>= '0' and <= '9' or >= 'a' and <= 'f'))))
                throw new InvalidDataException("Invalid native build profile");
        }
        return catalog;
    }

    internal static string ResourceRoot(string installation)
    {
        var root = Path.GetFullPath(installation);
        if (File.Exists(Path.Combine(root, ExecutableName))) return root;
        var resources = Path.Combine(root, "Contents", "Resources");
        if (File.Exists(Path.Combine(resources, ExecutableName))) return resources;
        throw new DirectoryNotFoundException("Choose a Windows game folder or Mac .app containing Crystal Project.exe");
    }

    internal static NativeBuild FromExecutable(string hash) =>
        Profiles.SingleOrDefault(profile => profile.ExecutableHash == hash) ??
        throw new InvalidDataException($"Unverified game executable; this release supports Windows and macOS {LatestVersion}. Update Crystal Goxel for a newer game build");

    internal static NativeBuild FromContext(JsonObject context)
    {
        var profile = FromExecutable(context["executableSha256"]?.GetValue<string>() ?? "");
        if (context["worldSha256"]?.GetValue<string>() != profile.WorldHash ||
            context["voxelSha256"]?.GetValue<string>() != profile.VoxelHash ||
            context["gameVersion"] is not null && context["gameVersion"]!.GetValue<string>() != profile.Label)
            throw new InvalidDataException("Native context mixes source fingerprints or version metadata");
        return profile;
    }

    internal static void RequireSame(JsonObject context, NativeBuild installation)
    {
        if (FromContext(context) != installation)
            throw new InvalidDataException("Installed game differs from this world context; restore its original resources or create a new cache for the selected build");
    }
}
