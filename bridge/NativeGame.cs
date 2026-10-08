// SPDX-License-Identifier: GPL-3.0-or-later
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;

namespace CrystalBridge;

internal sealed class NativeGame : IDisposable
{
    internal const string ReviewedExecutable = "36f7d413160a4deee36b47fc6ac534e87cadb6f23f57337d4630ec99cedb14e6";
    internal const string ReviewedWorld = "a93405dccaf04289f84d38be5b994d62b12e207afc8762d0bf79086023401107";
    internal const string ReviewedVoxels = "2767a8d1298dff7d44d9c440e7b8ff69e549c17a7fd0e01424b7da52123c137b";
    private const string ReviewedEditor = "315c0398646873fee6f61a8d01a0581b865b356ea9ae0d949672860200108a0f";
    internal const int EditorVersion = 34;
    internal const int LastVanillaEntity = 3824;
    private const int VertexCapacity = 2048;
    private readonly Assembly game;
    private readonly Type voxelInstance;
    private readonly Type vertexType;
    private readonly Type vectorType;
    private readonly Array builders;
    private readonly Array types;
    private readonly Array cache;
    private readonly Array vertices;
    private readonly FieldInfo instanceType;
    private readonly FieldInfo instanceParams;
    private readonly FieldInfo instanceLighting;
    private readonly FieldInfo position;
    private readonly FieldInfo uv;
    private readonly FieldInfo color;
    private readonly FieldInfo flags;
    private readonly object zero;
    internal readonly JsonElement[] Definitions;
    internal string Installation { get; }
    internal string ExecutableHash { get; }

    internal NativeGame(string installation)
    {
        Installation = Path.GetFullPath(installation);
        var executable = File.ReadAllBytes(Path.Combine(Installation, "Crystal Project.exe"));
        ExecutableHash = Hash(executable);
        if (ExecutableHash != ReviewedExecutable)
            throw new InvalidDataException("This build supports the inspected Windows Crystal Project 1.6.9.0 executable");
        AssemblyLoadContext.Default.Resolving += Resolve;
        try
        {
            game = HostAssembly("Crystal Project.exe");
            var cv = GameType("Voxel.CVoxel");
            voxelInstance = GameType("Voxel.VoxelInstance");
            vertexType = GameType("Gfx.VoxelVertex");
            var definitionType = GameType("Voxel.VoxelType");
            var graphics = GameType("SangServices").GetField("GraphicsData")!;
            graphics.SetValue(null, Activator.CreateInstance(graphics.FieldType, true));
            var fna = AssemblyLoadContext.Default.LoadFromAssemblyName(game.GetReferencedAssemblies().Single(a => a.Name == "FNA"));
            vectorType = fna.GetType("Microsoft.Xna.Framework.Vector3", true)!;
            zero = Activator.CreateInstance(vectorType)!;
            types = (Array)cv.GetField("TYPES")!.GetValue(null)!;
            var voxelPath = Path.Combine(Installation, "Content", "Database", "voxel.dat");
            if (Hash(File.ReadAllBytes(voxelPath)) != ReviewedVoxels) throw new InvalidDataException("Native voxel definitions differ from the inspected installation");
            Definitions = ReadDatabase(voxelPath);
            if (Definitions.Length != 256) throw new InvalidDataException("Unexpected native block database size");
            var options = new JsonSerializerOptions { IncludeFields = true };
            for (var id = 0; id < types.Length; id++)
            {
                var definition = Definitions[id];
                var value = definition.ValueKind == JsonValueKind.Null
                    ? Activator.CreateInstance(definitionType)!
                    : JsonSerializer.Deserialize(definition.GetRawText(), definitionType, options)!;
                // Native geometry expects initialized atlas UVs rather than the authored tile coordinates
                foreach (var key in new[] { "TexTopU", "TexTopV", "TexSideU", "TexSideV", "TexBotU", "TexBotV" })
                {
                    var field = definitionType.GetField(key)!;
                    field.SetValue(value, 1f / 432f + (float)field.GetValue(value)! / 24f);
                }
                types.SetValue(value, id);
            }
            cv.GetMethod("InitializePrimitiveBuilders", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null);
            builders = (Array)cv.GetField("PRIMITIVE_BUILDERS")!.GetValue(null)!;
            cache = Array.CreateInstance(voxelInstance, 3, 3, 3);
            vertices = Array.CreateInstance(vertexType, VertexCapacity);
            instanceType = voxelInstance.GetField("TypeID")!;
            instanceParams = voxelInstance.GetField("Params")!;
            instanceLighting = voxelInstance.GetField("Lighting")!;
            position = vertexType.GetField("Position")!;
            uv = vertexType.GetField("TextureCoordinate")!;
            color = vertexType.GetField("Color")!;
            flags = vertexType.GetField("Flags")!;
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    private Type GameType(string name) => game.GetType("Sang." + name, true)!;

    private Assembly? Resolve(AssemblyLoadContext context, AssemblyName name)
    {
        if (name.Name is null || !name.Name.All(c => char.IsLetterOrDigit(c) || c is '.' or '_' or '-')) return null;
        var path = Path.Combine(Installation, name.Name + ".dll");
        return File.Exists(path) ? HostAssembly(name.Name + ".dll") : null;
    }

    private Assembly HostAssembly(string name) => LoadManagedAssembly(
        AssemblyLoadContext.Default, Path.Combine(Installation, name),
        RuntimeInformation.ProcessArchitecture);

    internal static Assembly LoadManagedAssembly(AssemblyLoadContext context,
        string path, Architecture architecture)
    {
        var bytes = File.ReadAllBytes(path);
        if (architecture == Architecture.Arm64)
        {
            using var reader = new System.Reflection.PortableExecutable.PEReader(new MemoryStream(bytes));
            var header = reader.PEHeaders;
            if (header.CorHeader is null || (header.CorHeader.Flags & System.Reflection.PortableExecutable.CorFlags.ILOnly) == 0)
                throw new InvalidDataException("Native dependencies cannot be adapted to the helper process");
            var pe = BitConverter.ToInt32(bytes, 0x3c);
            var machine = BitConverter.ToUInt16(bytes, pe + 4);
            if (machine == 0x8664)
            {
                // Only private IL-only byte copies change; installed files and their hashes stay intact
                BitConverter.GetBytes((ushort)0xaa64).CopyTo(bytes, pe + 4);
            }
            else if (machine is not (0xaa64 or 0x14c)) throw new InvalidDataException("Unsupported managed assembly architecture");
        }
        // Path-based loading locks assemblies until process exit on Windows
        using var stream = new MemoryStream(bytes, writable: false);
        return context.LoadFromStream(stream);
    }

    internal static string Hash(byte[] bytes) => Convert.ToHexStringLower(SHA256.HashData(bytes));

    internal static JsonElement[] ReadDatabase(string path)
    {
        using var document = ReadDatabaseJson(path);
        return document.RootElement.EnumerateArray().Select(v => v.Clone()).ToArray();
    }

    internal static JsonDocument ReadDatabaseJson(string path)
    {
        var bytes = File.ReadAllBytes(path);
        if (bytes.Length is < 3 or > 16_000_000) throw new InvalidDataException("Invalid database size");
        var decoded = bytes[2..];
        for (var i = 0; i < decoded.Length; i++) decoded[i] ^= 255;
        return JsonDocument.Parse(decoded);
    }

    internal IEnumerable<NativeLandmark> HomePoints(byte[] bytes)
    {
        using var reader = new BinaryReader(new MemoryStream(bytes));
        if (reader.ReadByte() != 0) throw new InvalidDataException("Unsupported native entity chunk version");
        var count = reader.ReadInt32();
        if (count < 0 || count > LastVanillaEntity)
            throw new InvalidDataException("Invalid native entity chunk count");
        var entityType = GameType("SangEntity.SangEntityData");
        var deserialize = GameType("SangEntity.EntitySerializer").GetMethod("Deserialize", [typeof(BinaryReader), entityType.MakeByRefType()])!;
        for (var i = 0; i < count; i++)
        {
            object[] arguments = [reader, Activator.CreateInstance(entityType)!];
            deserialize.Invoke(null, arguments);
            var entity = arguments[1];
            if (entityType.GetField("EntityType")!.GetValue(entity)!.ToString() != "HomePoint") continue;
            var coord = entityType.GetField("Coord")!.GetValue(entity)!;
            var outfit = entityType.GetField("HomePointData")!.GetValue(entity)!;
            yield return new NativeLandmark(
                (int)entityType.GetField("ID")!.GetValue(entity)!,
                (string)outfit.GetType().GetField("Name")!.GetValue(outfit)!,
                (byte)entityType.GetField("BiomeID")!.GetValue(entity)!,
                new[] { "X", "Y", "Z" }.Select(axis => (int)coord.GetType().GetField(axis)!.GetValue(coord)!).ToArray());
        }
        if (reader.BaseStream.Position != reader.BaseStream.Length)
            throw new InvalidDataException("Native entity chunk has trailing data");
    }

    internal bool Visible(byte type) => Definitions[type].ValueKind == JsonValueKind.Object && Definitions[type].GetProperty("Visible").GetBoolean();

    internal void CheckEntities(string sourcePath, bool construction = false)
    {
        var editorPath = Path.Combine("Crystal Edit", "Crystal Edit.exe");
        if (Hash(File.ReadAllBytes(Path.Combine(Installation, editorPath))) != ReviewedEditor) throw new InvalidDataException("Crystal Edit executable differs from the inspected build");
        var editor = HostAssembly(editorPath);
        var json = AssemblyLoadContext.Default.LoadFromAssemblyName(editor.GetReferencedAssemblies().Single(a => a.Name == "Newtonsoft.Json"));
        var deserialize = json.GetType("Newtonsoft.Json.JsonConvert", true)!.GetMethod("DeserializeObject", [typeof(string), typeof(Type)])!;
        var model = editor.GetType("SangEdit.Models.Entities.ModelEntityData", true)!;
        var serialize = editor.GetType("SangEdit.Models.Entities.EntitySerializer", true)!.GetMethod("Serialize", [typeof(BinaryWriter), model])!;
        var runtimeType = GameType("SangEntity.SangEntityData");
        var gameDeserialize = GameType("SangEntity.EntitySerializer").GetMethod("Deserialize", [typeof(BinaryReader), runtimeType.MakeByRefType()])!;
        var project = Projects.Read(sourcePath);
        var constructionOutfits = new List<(int ID, object Outfit)>();
        if (construction && (project["Entities"]!.AsArray().Count == 0 || project["Entities"]!.AsArray().Any(node => node is not System.Text.Json.Nodes.JsonObject entity || !Projects.StaticVoxel(entity, out _, out _))))
            throw new InvalidDataException("check-construction requires a non-empty project containing only stationary, unconditional voxel NPCs");
        object[]? loadedEntities = null;
        // Non-empty model trees require editor UI registration unavailable to this offline host
        if (project["Tree"] is null or System.Text.Json.Nodes.JsonArray { Count: 0 })
        {
            var projectType = editor.GetType("SangEdit.Models.ModelMod", true)!;
            var loadedProject = deserialize.Invoke(null, [File.ReadAllText(sourcePath), projectType])!;
            if ((string)projectType.GetProperty("ID")!.GetValue(loadedProject)! != project["ID"]!.GetValue<string>() || (ulong)projectType.GetProperty("SteamWorkshopFileID")!.GetValue(loadedProject)! != (project["SteamWorkshopFileID"]?.GetValue<ulong>() ?? 0))
                throw new InvalidDataException("Crystal Edit project reader changed project identity");
            loadedEntities = ((System.Collections.IEnumerable)projectType.GetProperty("Entities")!.GetValue(loadedProject)!).Cast<object>().ToArray();
            if (loadedEntities.Length != project["Entities"]!.AsArray().Count) throw new InvalidDataException("Crystal Edit project reader changed entity count");
        }
        var count = 0;
        foreach (var node in project["Entities"]!.AsArray())
        {
            if (node is null) throw new InvalidDataException("Null entity in source project");
            var entity = loadedEntities is null ? deserialize.Invoke(null, [node.ToJsonString(), model])! : loadedEntities[count];
            using var stream = new MemoryStream();
            using (var writer = new BinaryWriter(stream, System.Text.Encoding.UTF8, true)) serialize.Invoke(null, [writer, entity]);
            stream.Position = 0;
            using var reader = new BinaryReader(stream);
            object[] arguments = [reader, Activator.CreateInstance(runtimeType)!];
            gameDeserialize.Invoke(null, arguments);
            var runtimeEntity = arguments[1];
            if (stream.Position != stream.Length || (int)runtimeEntity.GetType().GetField("ID")!.GetValue(runtimeEntity)! != node["ID"]!.GetValue<int>())
                throw new InvalidDataException("Crystal Edit / game entity round trip changed identity");
            var coord = runtimeType.GetField("Coord")!.GetValue(runtimeEntity)!;
            foreach (var axis in new[] { "X", "Y", "Z" })
                if ((int)coord.GetType().GetField(axis)!.GetValue(coord)! != node["Coord"]![axis]!.GetValue<int>()) throw new InvalidDataException("Entity round trip changed world coordinates");
            if (node["NpcData"]?["Outfits"] is System.Text.Json.Nodes.JsonArray outfits)
            {
                var npc = runtimeType.GetField("NpcData")!.GetValue(runtimeEntity)!;
                var compiled = (Array)npc.GetType().GetField("Outfits")!.GetValue(npc)!;
                if (compiled.Length != outfits.Count) throw new InvalidDataException("Entity round trip changed outfit count");
                for (var i = 0; i < outfits.Count; i++)
                {
                    var outfit = compiled.GetValue(i)!;
                    if ((byte?)outfit.GetType().GetField("VoxelID")!.GetValue(outfit) != outfits[i]?["VoxelID"]?.GetValue<byte>() || (byte)outfit.GetType().GetField("VoxelVariantIndex")!.GetValue(outfit)! != (outfits[i]?["VoxelVariantIndex"]?.GetValue<byte>() ?? 0))
                        throw new InvalidDataException("Entity round trip changed block identity");
                    foreach (var key in new[] { "MountType", "PlayerCollision", "NpcCollision", "WanderType", "JumpType" })
                        if (outfits[i]?[key] is not null && Convert.ToInt32(outfit.GetType().GetField(key)!.GetValue(outfit)) != outfits[i]![key]!.GetValue<int>())
                            throw new InvalidDataException("Entity round trip changed physics setting: " + key);
                    foreach (var key in new[] { "KeepSpawned", "AutoStep" })
                        if (outfits[i]?[key] is not null && (bool)outfit.GetType().GetField(key)!.GetValue(outfit)! != outfits[i]![key]!.GetValue<bool>())
                            throw new InvalidDataException("Entity round trip changed physics setting: " + key);
                    if (construction) constructionOutfits.Add((node["ID"]!.GetValue<int>(), outfit));
                }
            }
            count++;
        }
        object? physicsResult = null;
        if (construction)
        {
            var physics = new NativePhysics(GameType, vectorType);
            var solid = constructionOutfits.Count(item => physics.Check(item.Outfit, item.ID));
            physicsResult = new { airMotion = "ok", liquidMotion = "ok", solidBoxContacts = solid == 0 ? "notApplicable" : "ok", solid, decorative = constructionOutfits.Count - solid };
        }
        Console.WriteLine(JsonSerializer.Serialize(new { entities = count, editorProjectRead = loadedEntities is null ? "notChecked" : "ok", editorSerialization = "ok", gameDeserialization = "ok", constructionPhysics = physicsResult }));
    }

    internal NativeVertex[] Build(Cell center, Func<int, int, int, Cell> neighbor)
    {
        if (!Visible(center.Type)) return [];
        var builderIndex = Definitions[center.Type].GetProperty("PrimitiveBuilder").GetInt32();
        if (builderIndex < 0 || builderIndex >= builders.Length || builders.GetValue(builderIndex) is not object builder)
            throw new InvalidDataException($"Missing native builder for block {center.Type}");
        for (var x = 0; x < 3; x++)
            for (var y = 0; y < 3; y++)
                for (var z = 0; z < 3; z++)
                {
                    var cell = x == 1 && y == 1 && z == 1 ? center : neighbor(x - 1, y - 1, z - 1);
                    var value = Activator.CreateInstance(voxelInstance)!;
                    instanceType.SetValue(value, cell.Type);
                    instanceParams.SetValue(value, cell.Parameters);
                    instanceLighting.SetValue(value, cell.Light);
                    cache.SetValue(value, x, y, z);
                }
        object[] arguments = [vertices, 0, zero, cache];
        try { builder.GetType().GetMethod("Build")!.Invoke(builder, arguments); }
        catch (TargetInvocationException e) { throw new InvalidDataException($"Native block builder {builderIndex} failed", e.InnerException); }
        var count = (int)arguments[1];
        if (count < 0 || count > VertexCapacity || count % 4 != 0) throw new InvalidDataException("Unexpected native quad layout");
        var result = new NativeVertex[count];
        for (var i = 0; i < count; i++)
        {
            var vertex = vertices.GetValue(i)!;
            var p = position.GetValue(vertex)!;
            var t = uv.GetValue(vertex)!;
            var c = color.GetValue(vertex)!;
            float F(object value, string key) => (float)value.GetType().GetField(key)!.GetValue(value)!;
            byte C(string key) => (byte)c.GetType().GetProperty(key)!.GetValue(c)!;
            result[i] = new NativeVertex(F(p, "X"), F(p, "Y"), F(p, "Z"), F(t, "X"), F(t, "Y"), C("R"), C("G"), C("B"), C("A"), (int)(float)flags.GetValue(vertex)! & 7);
        }
        return result;
    }

    public void Dispose()
    {
        AssemblyLoadContext.Default.Resolving -= Resolve;
    }
}

internal readonly record struct Cell(byte Type, byte Parameters, ushort Light)
{
    internal static readonly Cell Air = new(0, 0, 15);
}

internal readonly record struct NativeVertex(float X, float Y, float Z, float U, float V, byte R, byte G, byte B, byte W, int Facing);
