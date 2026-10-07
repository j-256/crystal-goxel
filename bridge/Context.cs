// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;

namespace CrystalBridge;

internal static class Context
{
    internal static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    internal static int[] Triple(string value) => value.Split(',').Select(int.Parse).ToArray() is { Length: 3 } result
        ? result : throw new ArgumentException("Coordinates require three comma-separated integers");

    internal static int[] ToEditor(int[] world, int[] origin) => [world[0] - origin[0], origin[2] - world[2] - 1, world[1] - origin[1]];
    internal static int[] ToWorld(int[] cell, int[] origin) => [origin[0] + cell[0], origin[1] + cell[2], origin[2] - cell[1] - 1];

    internal static void Prepare(string installation, int[] center, int[] size, string output)
    {
        if (size.Any(v => v < 8 || v > 96) || (long)size[0] * size[1] * size[2] > 300_000)
            throw new ArgumentException("Region dimensions must be 8..96 with at most 300000 cells");
        if (center.Any(v => v < -10000 || v > 10000) || center[1] < 0 || center[1] > 255)
            throw new ArgumentException("Center is outside the supported native world bounds");
        if (Directory.Exists(output) || File.Exists(output)) throw new ArgumentException("Output already exists; choose a new context directory");
        using var game = new NativeGame(installation);
        var world = new NativeWorld(Path.Combine(game.Installation, "Content", "Worlds", "field.dat"));
        // The reviewed entity namespace is valid only for this exact native world
        if (world.WorldHash != NativeGame.ReviewedWorld) throw new InvalidDataException("Native world differs from the inspected Windows 1.6.9.0 installation");
        var origin = center.Zip(size, (c, s) => c - s / 2).ToArray();
        var staging = Path.GetFullPath(output) + "." + Guid.NewGuid().ToString("N") + ".partial";
        Directory.CreateDirectory(staging);
        try
        {
            var atlas = Atlas(Path.Combine(game.Installation, "Content", "Textures", "Voxel.dat"));
            File.WriteAllBytes(Path.Combine(staging, "atlas.png"), atlas);
            var blocks = new List<object>();
            using (var palette = new BinaryWriter(File.Create(Path.Combine(staging, "palette.mesh"))))
            {
                palette.Write("CGP1"u8);
                for (var id = 1; id < 256; id++)
                {
                    if (!game.Visible((byte)id)) continue;
                    var definition = game.Definitions[id];
                    var maxVariant = definition.GetProperty("MaxVariant").GetInt32();
                    if (maxVariant < 0 || maxVariant > 3) throw new InvalidDataException("Unexpected block variant limit");
                    blocks.Add(new { id, name = definition.GetProperty("Name").GetString(), maxVariant, collides = definition.GetProperty("Collides").GetBoolean() });
                    for (var variant = 0; variant <= maxVariant; variant++)
                    {
                        var vertices = game.Build(new Cell((byte)id, (byte)(variant << 6), ushort.MaxValue), (_, _, _) => Cell.Air);
                        palette.Write(id); palette.Write(variant); palette.Write(vertices.Length / 4 * 6);
                        WriteVertices(palette, vertices, 0, 0, 0, true);
                    }
                }
            }
            var visible = 0;
            using var biome = new BinaryWriter(File.Create(Path.Combine(staging, "biomes.bin")));
            using var owners = new BinaryWriter(File.Create(Path.Combine(staging, "reference.cells")));
            owners.Write("CGC1"u8); owners.Write(0);
            var ownerCount = 0;
            using (var mesh = new BinaryWriter(File.Create(Path.Combine(staging, "reference.mesh"))))
            {
                mesh.Write("CGM1"u8); mesh.Write(0);
                var count = 0;
                for (var x = origin[0]; x < origin[0] + size[0]; x++)
                    for (var y = origin[1]; y < origin[1] + size[1]; y++)
                        for (var z = origin[2]; z < origin[2] + size[2]; z++)
                        {
                            biome.Write(world.Biome(x, y, z));
                            var cell = world.Get(x, y, z);
                            if (!game.Visible(cell.Type)) continue;
                            var vertices = game.Build(cell, (dx, dy, dz) => world.Get(x + dx, y + dy, z + dz));
                            if (vertices.Length == 0) continue;
                            visible++;
                            WriteVertices(mesh, vertices, x - origin[0], y - origin[1], z - origin[2], false);
                            count += vertices.Length / 4 * 6;
                            var owner = ToEditor([x, y, z], origin);
                            for (var triangle = 0; triangle < vertices.Length / 4 * 2; triangle++)
                            {
                                foreach (var coordinate in owner) owners.Write(coordinate);
                                ownerCount++;
                            }
                        }
                mesh.Seek(4, SeekOrigin.Begin); mesh.Write(count);
            }
            biome.Dispose();
            owners.Seek(4, SeekOrigin.Begin); owners.Write(ownerCount); owners.Dispose();
            var files = new[] { "reference.mesh", "reference.cells", "palette.mesh", "atlas.png", "biomes.bin" }.ToDictionary(n => n, n => NativeGame.Hash(File.ReadAllBytes(Path.Combine(staging, n))));
            var manifest = new
            {
                format = 1,
                gameVersion = "Windows 1.6.9.0",
                executableSha256 = game.ExecutableHash,
                worldSha256 = world.WorldHash,
                voxelSha256 = NativeGame.Hash(File.ReadAllBytes(Path.Combine(game.Installation, "Content", "Database", "voxel.dat"))),
                origin,
                size,
                blocks,
                visible,
                files
            };
            File.WriteAllText(Path.Combine(staging, "context.json"), JsonSerializer.Serialize(manifest, JsonOptions));
            Directory.Move(staging, Path.GetFullPath(output));
            Console.WriteLine(JsonSerializer.Serialize(new { context = Path.Combine(Path.GetFullPath(output), "context.json"), visible }));
        }
        catch { Directory.Delete(staging, true); throw; }
    }

    private static void WriteVertices(BinaryWriter writer, NativeVertex[] vertices, int x, int y, int z, bool template)
    {
        ReadOnlySpan<int> triangles = [0, 1, 2, 2, 1, 3];
        for (var q = 0; q < vertices.Length; q += 4)
            foreach (var index in triangles)
            {
                var v = vertices[q + index];
                // Rotate the game's Y-up world into Goxel's Z-up frame without reflecting its geometry
                writer.Write(v.X + x); writer.Write((template ? 1 : 0) - v.Z - z); writer.Write(v.Y + y);
                var normal = v.Facing switch { 0 => (-1f, 0f, 0f), 1 => (0f, 0f, -1f), 2 => (0f, 1f, 0f), 3 => (1f, 0f, 0f), 4 => (0f, 0f, 1f), _ => (0f, -1f, 0f) };
                writer.Write(normal.Item1); writer.Write(normal.Item2); writer.Write(normal.Item3);
                // Native alpha stores sunlight, not opacity; atlas alpha supplies the actual transparency
                byte Light(byte channel) => template ? (byte)255 : (byte)Math.Clamp(60 + channel + v.W * 0.72f, 60, 255);
                writer.Write(Light(v.R)); writer.Write(Light(v.G)); writer.Write(Light(v.B)); writer.Write((byte)255);
                writer.Write(v.U); writer.Write(v.V);
            }
    }

    private static byte[] Atlas(string path)
    {
        using var reader = new BinaryReader(File.OpenRead(path));
        reader.ReadUInt16();
        var count = reader.ReadUInt32();
        if (count > 10000) throw new InvalidDataException("Invalid texture pack count");
        reader.BaseStream.Seek(count * 7, SeekOrigin.Current);
        for (var i = 0; i < count; i++)
        {
            var nameLength = reader.ReadUInt32();
            if (nameLength > 4096) throw new InvalidDataException("Invalid texture name length");
            var name = System.Text.Encoding.UTF8.GetString(reader.ReadBytes((int)nameLength));
            var length = reader.ReadUInt32();
            if (length > 16_000_000) throw new InvalidDataException("Texture exceeds its limit");
            var bytes = reader.ReadBytes((int)length);
            if (bytes.Length != length) throw new InvalidDataException("Truncated texture pack");
            if (name == "VoxelAtlas") return bytes;
        }
        throw new InvalidDataException("VoxelAtlas is missing from the selected game");
    }
}
