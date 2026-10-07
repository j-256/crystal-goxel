// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using CrystalBridge;

const string Help = """
Usage: crystal-bridge COMMAND [OPTIONS]
Offline Crystal Project native world context and Crystal Edit project bridge

  prepare --game DIR --center X,Y,Z --size X,Y,Z --output NEW_DIR
  import  --context context.json --source PROJECT.json --output snapshot.json
  export  --context context.json --snapshot snapshot.json --output NEW_PROJECT.json
  validate --context context.json
  check-entities --game DIR --source PROJECT.json
  check-construction --game DIR --source PROJECT.json
  test    Run synthetic preservation and coordinate checks

Options accept --name VALUE or --name=VALUE. Paths may contain spaces.
Short options: -g game, -c center, -s size, -o output, -k context,
-i source/snapshot, -h help. Short values can be glued to their option.
Exit status: 0 success, 1 runtime failure, 2 usage/precondition, 3 missing dependency
Game resources stay local. Exports are Crystal Edit voxel NPCs, with native terrain
kept as reference. Only the inspected Windows 1.6.9.0 installation is supported.
check-construction requires only stationary, unconditional voxel NPCs. It checks
the original readers, air/liquid motion and solid player contacts in CPU fixtures.
Export snapshots use format 1 with source text, managed mappings and authored
cells. Optional newObjectsSolid is boolean, defaults to true, and affects new NPCs.
These checks do not launch the game or validate a complete gameplay session.
""";
if (args.Length == 0)
{
    Console.Error.WriteLine(Help);
    return 2;
}
if (args[0] is "--help" or "-h")
{
    Console.WriteLine(Help);
    return 0;
}

var clock = Stopwatch.StartNew();
var correlation = Guid.NewGuid().ToString("N")[..12];
try
{
    var options = new Dictionary<string, string>();
    var tokens = new List<string>();
    var ended = false;
    foreach (var argument in args.Skip(1))
    {
        if (ended || argument.Length < 2 || !argument.StartsWith('-') || !char.IsLetter(argument[1]))
        { tokens.Add(argument); if (argument == "--") ended = true; continue; }
        if (argument[1] == 'h') { tokens.Add("--help"); continue; }
        var option = argument[1] switch { 'g' => "game", 'c' => "center", 's' => "size", 'o' => "output", 'k' => "context", 'i' => args[0] == "export" ? "snapshot" : "source", _ => throw new ArgumentException("Unknown short option") };
        tokens.Add("--" + option);
        if (argument.Length > 2) tokens.Add(argument[2..]);
    }
    for (var i = 0; i < tokens.Count; i++)
    {
        if (tokens[i] == "--")
        { if (i + 1 != tokens.Count) throw new ArgumentException("This command does not accept positional arguments"); break; }
        if (tokens[i] == "--help") { Console.WriteLine(Help); return 0; }
        var parts = tokens[i].Split('=', 2);
        if (!parts[0].StartsWith("--")) throw new ArgumentException("Unexpected positional argument");
        var value = parts.Length == 2 ? parts[1] : ++i < tokens.Count ? tokens[i] : throw new ArgumentException("Missing option value");
        if (value.Length == 0 || !options.TryAdd(parts[0][2..], value)) throw new ArgumentException("Empty or duplicate option");
    }
    string Required(string key) => options.Remove(key, out var value) ? value : throw new ArgumentException("Missing --" + key);
    switch (args[0])
    {
        case "prepare":
            {
                var game = Required("game"); var center = Context.Triple(Required("center"));
                var size = Context.Triple(Required("size")); var output = Required("output");
                if (options.Count != 0) throw new ArgumentException("Unknown option");
                Context.Prepare(game, center, size, output); break;
            }
        case "import":
        case "export":
            {
                var context = Required("context"); var input = Required(args[0] == "import" ? "source" : "snapshot"); var output = Required("output");
                if (options.Count != 0) throw new ArgumentException("Unknown option");
                if (args[0] == "import") Projects.Import(context, input, output); else Projects.Export(context, input, output);
                break;
            }
        case "test":
            if (options.Count != 0) throw new ArgumentException("Unknown option");
            Projects.Test(); break;
        case "validate":
            {
                var path = Required("context");
                if (options.Count != 0) throw new ArgumentException("Unknown option");
                Projects.ValidateContext(path, Projects.Read(path));
                Console.WriteLine("Context fingerprints verified"); break;
            }
        case "check-entities":
        case "check-construction":
            {
                var path = Required("game"); var source = Required("source");
                if (options.Count != 0) throw new ArgumentException("Unknown option");
                using var game = new NativeGame(path);
                game.CheckEntities(source, args[0] == "check-construction"); break;
            }
        default: throw new ArgumentException("Unknown command; see --help");
    }
    Console.Error.WriteLine($"crystal-bridge operation={args[0]} result=ok elapsedMs={clock.ElapsedMilliseconds} id={correlation}");
    return 0;
}
catch (Exception error)
{
    error = error.GetBaseException();
    var exit = error is ArgumentException or FormatException or OverflowException or InvalidDataException ? 2 : error is FileNotFoundException or DirectoryNotFoundException ? 3 : 1;
    Console.Error.WriteLine($"crystal-bridge operation={args[0]} result=error exit={exit} elapsedMs={clock.ElapsedMilliseconds} id={correlation} message={error.Message}");
    return exit;
}
