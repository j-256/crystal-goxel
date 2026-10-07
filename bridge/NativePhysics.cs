// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections;
using System.Reflection;

namespace CrystalBridge;

internal sealed class NativePhysics
{
    private const int MotionTicks = 120;
    private const float InitialHeight = 100f;
    private const float ContactGap = 0.02f;
    private const float ContactSpeed = 0.1f;
    private readonly Type entityType;
    private readonly Type vectorType;
    private readonly Array presets;
    private readonly object playerRadius;
    private readonly object voxelRadius;
    private readonly object engine;
    private readonly MethodInfo applyPhysics;
    private readonly MethodInfo applyForce;
    private readonly MethodInfo resetTick;
    private readonly MethodInfo applyTick;
    private readonly MethodInfo collide;
    private readonly int noCollision;
    private readonly int fullCollision;
    private readonly int collisionFrom;
    private readonly int playerMount;

    internal NativePhysics(Func<string, Type> type, Type vectorType)
    {
        this.vectorType = vectorType;
        var field = type("Field.CField");
        var cache = type("Gfx.Cache").GetField("_textures", BindingFlags.Static | BindingFlags.NonPublic)!;
        var previous = cache.GetValue(null);
        var textures = (IDictionary)Activator.CreateInstance(cache.FieldType)!;
        // The original CPU preset initializer only stores texture references
        // Null fallback entries avoid loading textures or creating a GPU device
        textures.Add("System/NullTexture", null);
        textures.Add("System/InvisibleTexture", null);
        try
        {
            cache.SetValue(null, textures);
            field.GetMethod("InitializePhysics", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null);
        }
        finally { cache.SetValue(null, previous); }
        presets = (Array)field.GetField("PHYS")!.GetValue(null)!;
        playerRadius = field.GetField("PLAYER_RADIUS")!.GetValue(null)!;
        voxelRadius = field.GetField("NPC_VOXEL_RADIUS")!.GetValue(null)!;
        entityType = type("Field.PhysicsEntity");
        var tickType = type("Field.PhysicsEntityNextTick");
        applyPhysics = entityType.GetMethod("ApplyPhysics", [type("Field.PhysicsPreset").MakeByRefType()])!;
        applyForce = entityType.GetMethod("ApplyForce")!;
        resetTick = tickType.GetMethod("Reset")!;
        applyTick = tickType.GetMethod("Apply")!;
        var engineType = type("Field.PhysicsEngine");
        engine = Activator.CreateInstance(engineType, true)!;
        collide = engineType.GetMethod("ApplyBoxCollisionDynamic", BindingFlags.Instance | BindingFlags.NonPublic)!;
        var collisionType = type("SangEntity.SangEntityEnums.CollisionType");
        noCollision = Convert.ToInt32(Enum.Parse(collisionType, "None"));
        fullCollision = Convert.ToInt32(Enum.Parse(collisionType, "Full"));
        collisionFrom = Convert.ToInt32(Enum.Parse(collisionType, "OneWayFrom"));

        // A walking NPC must fall in this fixture, proving the native force
        // and integration methods are running rather than a no-op test host
        var mountType = type("Field.Enums.MountType");
        playerMount = Convert.ToInt32(Enum.Parse(mountType, "PlayerFoot"));
        var walking = Convert.ToInt32(Enum.Parse(mountType, "NpcFoot"));
        Require(Motion(Body(Vector(0, InitialHeight, 0), voxelRadius, walking), false) < InitialHeight, "walking NPC negative control did not fall");
    }

    internal bool Check(object outfit, int entityID)
    {
        var mount = Convert.ToInt32(Get(outfit, "MountType"));
        var playerCollision = Convert.ToInt32(Get(outfit, "PlayerCollision"));
        var npcCollision = Convert.ToInt32(Get(outfit, "NpcCollision"));
        var solid = playerCollision == fullCollision && npcCollision == fullCollision;
        Require(solid || playerCollision == noCollision && npcCollision == noCollision, $"entity {entityID}: construction needs full or no collision for both player and NPCs");
        Require(Convert.ToInt32(Get(outfit, "WanderType")) == 0 && Convert.ToInt32(Get(outfit, "JumpType")) == 0 && (bool)Get(outfit, "KeepSpawned"), $"entity {entityID}: construction must stay spawned without wandering or jumping");
        foreach (var wet in new[] { false, true })
        {
            var body = Body(Vector(0, InitialHeight, 0), voxelRadius, mount);
            var height = Motion(body, wet);
            Require(height == InitialHeight, $"entity {entityID}: native {(wet ? "liquid" : "air")} physics moved construction from {InitialHeight} to {height}");
        }
        if (solid)
        {
            Require((playerCollision & collisionFrom) != 0, $"entity {entityID}: player collision is disabled");
            foreach (var axis in new[] { "X", "Y", "Z" })
                foreach (var direction in new[] { -1f, 1f }) Contact(mount, axis, direction, entityID);
        }
        return solid;
    }

    private void Contact(int mount, string axis, float direction, int entityID)
    {
        var npc = Body(Vector(0, InitialHeight, 0), voxelRadius, mount);
        var origin = axis == "Y" ? InitialHeight : 0f;
        var position = Vector(0, InitialHeight, 0);
        Set(position, axis, origin - direction * ((float)Get(voxelRadius, axis) + (float)Get(playerRadius, axis) + ContactGap));
        var player = Body(position, playerRadius, playerMount);
        var velocity = Vector(0, 0, 0);
        Set(velocity, axis, direction * ContactSpeed);
        Set(player, "Vel", velocity);
        var tick = Get(player, "NextTick");
        resetTick.Invoke(tick, [player]);
        var box = Get(npc, "Box");
        var overlap = box.GetType().GetMethod("TestPartialOverlap")!;
        Require((bool)overlap.Invoke(box, [Get(tick, "NewBox")])!, "contact fixture did not overlap construction");
        collide.Invoke(engine, [player, box, npc]);
        applyTick.Invoke(tick, [player]);
        Require((float)Get(Get(player, "Vel"), axis) == 0f && (bool)Get(tick, "Stop" + axis), $"entity {entityID}: native collision failed for {axis} direction {direction}");
        Require(direction * ((float)Get(Get(player, "Pos"), axis) - origin) < -((float)Get(voxelRadius, axis) + (float)Get(playerRadius, axis)), "native collision did not separate the player and construction");
        if (axis == "Y" && direction < 0)
            Require((bool)Get(player, "IsGrounded") && ReferenceEquals(Get(player, "GroundingEntity"), npc), $"entity {entityID}: player could not stand on construction");
        Require((float)Get(Get(npc, "Pos"), "Y") == InitialHeight && (float)Get(Get(npc, "Vel"), "Y") == 0f, "player contact moved construction");
    }

    private object Body(object position, object radius, int mount)
    {
        Require(mount >= 0 && mount < presets.Length, "unsupported native mount preset");
        // The fixture uses no world contacts; these methods only need the body
        // and its next-tick state, not a world buffer, audio or renderer
        var body = Activator.CreateInstance(entityType, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, [null, null], null)!;
        Set(body, "Pos", position);
        Set(body, "_friction", 1f);
        entityType.GetMethod("SetRadius")!.Invoke(body, [radius]);
        applyPhysics.Invoke(body, [presets.GetValue(mount)]);
        return body;
    }

    private float Motion(object body, bool wet)
    {
        var tick = Get(body, "NextTick");
        for (var i = 0; i < MotionTicks; i++)
        {
            Set(body, "IsWet", wet);
            applyForce.Invoke(body, null);
            resetTick.Invoke(tick, [body]);
            Set(tick, "NewIsWet", wet);
            applyTick.Invoke(tick, [body]);
        }
        return (float)Get(Get(body, "Pos"), "Y");
    }

    private object Vector(float x, float y, float z) => Activator.CreateInstance(vectorType, [x, y, z])!;
    private static FieldInfo Field(object value, string name) => value.GetType().GetField(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic) ?? throw new MissingFieldException(value.GetType().FullName, name);
    private static object Get(object value, string name) => Field(value, name).GetValue(value)!;
    private static void Set(object value, string name, object field) => Field(value, name).SetValue(value, field);
    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidDataException("Construction physics check failed: " + message);
    }
}
