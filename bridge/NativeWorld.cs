// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Compression;

namespace CrystalBridge;

internal sealed class NativeWorld
{
    private const int MaxBytes = 64 * 1024 * 1024;
    private readonly byte[] data;
    private readonly Dictionary<(int X, int Z), (int Offset, int Length)> regions = [];
    private readonly Dictionary<(int X, int Y, int Z), Cell[]> chunks = [];
    private readonly Dictionary<(int X, int Y, int Z), byte[]> biomes = [];
    internal string WorldHash { get; }

    internal NativeWorld(string path)
    {
        var bytes = File.ReadAllBytes(path);
        if (bytes.Length is < 2 or > MaxBytes || bytes[0] != 5 || bytes[1] != 0)
            throw new InvalidDataException("Unsupported world archive header");
        WorldHash = NativeGame.Hash(bytes);
        using var zip = new ZipArchive(new MemoryStream(bytes, 2, bytes.Length - 2), ZipArchiveMode.Read);
        var entry = zip.GetEntry("field.dat") ?? throw new InvalidDataException("World archive has no field data");
        using var reader = new BinaryReader(new MemoryStream(Expand(entry)));
        Exact(reader, Count(reader, MaxBytes));
        Exact(reader, Count(reader, MaxBytes));
        var count = Count(reader, 100_000);
        for (var i = 0; i < count; i++)
        {
            var x = reader.ReadInt32(); var z = reader.ReadInt32();
            var offset = reader.ReadInt32(); var length = reader.ReadInt32();
            if (offset < 0 || length < 0 || !regions.TryAdd((x, z), (offset, length)))
                throw new InvalidDataException("Invalid or duplicate world region");
        }
        reader.ReadInt32();
        data = Exact(reader, Count(reader, MaxBytes));
        if (reader.BaseStream.Position != reader.BaseStream.Length || regions.Values.Any(r => (long)r.Offset + r.Length > data.Length))
            throw new InvalidDataException("World region data exceeds its container");
    }

    internal Cell Get(int x, int y, int z)
    {
        if (y < 0 || y >= 256) return Cell.Air;
        var cx = (int)Math.Floor(x / 16d); var cy = y / 16; var cz = (int)Math.Floor(z / 16d);
        if (!chunks.TryGetValue((cx, cy, cz), out var cells))
        {
            cells = new Cell[4096];
            biomes[(cx, cy, cz)] = new byte[8];
            if (regions.TryGetValue((cx, cz), out var region))
            {
                using var zip = new ZipArchive(new MemoryStream(data, region.Offset, region.Length), ZipArchiveMode.Read);
                var entry = zip.GetEntry($"y{cy}.dat");
                if (entry is not null)
                {
                    var bytes = Expand(entry);
                    if (bytes.Length != 16_397 || bytes[0] is not (7 or 8)) throw new InvalidDataException("Unsupported voxel chunk layout");
                    biomes[(cx, cy, cz)] = bytes[5..13];
                    for (var i = 0; i < cells.Length; i++)
                        cells[i] = new Cell(bytes[13 + i * 4], bytes[14 + i * 4], BitConverter.ToUInt16(bytes, 15 + i * 4));
                }
            }
            chunks[(cx, cy, cz)] = cells;
        }
        // Native chunks are X-major, then Y, then Z; floor division also preserves negative world coordinates
        return cells[(x - cx * 16) * 256 + (y - cy * 16) * 16 + z - cz * 16];
    }

    internal byte Biome(int x, int y, int z)
    {
        if (y < 0 || y >= 256) return 0;
        Get(x, y, z);
        var cx = (int)Math.Floor(x / 16d); var cz = (int)Math.Floor(z / 16d);
        return biomes[(cx, y / 16, cz)][(x - cx * 16) / 8 * 4 + y % 16 / 8 * 2 + (z - cz * 16) / 8];
    }

    internal IEnumerable<byte[]> EntityChunks()
    {
        // Entity entries are separate from terrain; use the original reader for their variable layouts
        foreach (var region in regions.OrderBy(r => r.Key.X).ThenBy(r => r.Key.Z))
        {
            using var zip = new ZipArchive(new MemoryStream(data, region.Value.Offset, region.Value.Length), ZipArchiveMode.Read);
            foreach (var entry in zip.Entries.OrderBy(e => e.FullName, StringComparer.Ordinal))
            {
                if (!entry.FullName.EndsWith("e.dat", StringComparison.Ordinal)) continue;
                if (!entry.FullName.StartsWith('y') ||
                    !int.TryParse(entry.FullName[1..^5], out var y) || y is < 0 or >= 16)
                    throw new InvalidDataException("Unsupported native entity chunk name");
                yield return Expand(entry);
            }
        }
    }

    private static int Count(BinaryReader reader, int limit)
    {
        var value = reader.ReadInt32();
        if (value < 0 || value > limit) throw new InvalidDataException("Invalid world data length");
        return value;
    }

    private static byte[] Exact(BinaryReader reader, int count)
    {
        var bytes = reader.ReadBytes(count);
        if (bytes.Length != count) throw new InvalidDataException("Truncated world data");
        return bytes;
    }

    private static byte[] Expand(ZipArchiveEntry entry)
    {
        if (entry.Length > MaxBytes) throw new InvalidDataException("Expanded world entry exceeds its limit");
        using var reader = new BinaryReader(entry.Open());
        return Exact(reader, checked((int)entry.Length));
    }
}
