#!/usr/bin/env python3
"""Dump / patch the Urban Terror 4.3.4 weapon damage table in qagame.qvm.

  utweapons.py dump  qagame.qvm weapons.json
  utweapons.py apply qagame.qvm weapons.json out/qagame.qvm [--pk3 zzz_mod.pk3]
  utweapons.py cfg   qagame.qvm luxmod.cfg        (stock-value reference for luxmod)

Table layout (found by reversing 4.3.4's qagame.qvm):
  base 0xfb4 in DATA, 432 bytes per weapon id, indexed by weapon number.
  +0    char *name        (pointer into LIT)
  +4    char *model name
  +48   15 x { float damage; int bleeds; }   indexed by hit location
        damage is a fraction of 100 HP, e.g. 0.44 = 44 hp
"""
import json
import struct
import sys
import zipfile

sys.path.insert(0, __import__("os").path.dirname(__file__))
from qvmdis import QVM  # noqa: E402

TABLE = 0xfb4
STRIDE = 432
NUM_WEAPONS = 28
HIT_OFF = 48
HITLOCS = ["Generic", "Head", "Helmet", "Torso", "Vest", "Left Arm",
           "Right Arm", "Groin", "Butt", "Left Upper Leg", "Right Upper Leg",
           "Left Lower Leg", "Right Lower Leg", "Left Foot", "Right Foot"]
EXPECTED_SIZE = 983300  # 4.3.4 qagame.qvm; other builds have other offsets


def entries(vm):
    for w in range(1, NUM_WEAPONS):
        e = TABLE + w * STRIDE
        name = vm.string_at(vm.word(e))
        if name:
            yield w, e, name


def dump(qvm_path, json_path):
    vm = QVM(qvm_path)
    out = {}
    for w, e, name in entries(vm):
        locs = {}
        for i, loc in enumerate(HITLOCS):
            dmg = struct.unpack_from("<f", vm.mem, e + HIT_OFF + 8 * i)[0]
            bleed = vm.word(e + HIT_OFF + 8 * i + 4)
            locs[loc] = {"damage": round(dmg * 100, 2), "bleeds": bool(bleed)}
        out[name] = {"id": w, "hitlocs": locs}
    json.dump(out, open(json_path, "w"), indent=2)
    print("wrote %d weapons to %s" % (len(out), json_path))


def apply(qvm_path, json_path, out_path, pk3=None):
    vm = QVM(qvm_path)
    if len(vm.raw) != EXPECTED_SIZE:
        sys.exit("unexpected qagame.qvm size %d; offsets are for 4.3.4" % len(vm.raw))
    raw = bytearray(vm.raw)
    cfg = json.load(open(json_path))
    names = {name: (w, e) for w, e, name in entries(vm)}
    changes = 0
    for name, wcfg in cfg.items():
        if name not in names:
            sys.exit("unknown weapon %r (known: %s)" % (name, ", ".join(names)))
        w, e = names[name]
        for loc, v in wcfg["hitlocs"].items():
            i = HITLOCS.index(loc)
            foff = vm.data_off + e + HIT_OFF + 8 * i
            old = struct.unpack_from("<fi", raw, foff)
            new = (v["damage"] / 100.0, int(bool(v["bleeds"])))
            if abs(old[0] - new[0]) > 1e-6 or old[1] != new[1]:
                struct.pack_into("<fi", raw, foff, *new)
                print("%-22s %-16s %6.1f%s -> %6.1f%s" % (
                    name, loc, old[0] * 100, "b" if old[1] else " ",
                    new[0] * 100, "b" if new[1] else " "))
                changes += 1
    open(out_path, "wb").write(raw)
    print("%d changes, wrote %s" % (changes, out_path))
    if pk3:
        with zipfile.ZipFile(pk3, "w", zipfile.ZIP_DEFLATED) as z:
            z.write(out_path, "vm/qagame.qvm")
        print("wrote %s" % pk3)


PROJECTILE_TYPES = (17, 19, 24, 26)

CFG_HEADER = """\
// luxmod config - Urban Terror 4.3.4
// by LuxXx - https://github.com/LuxXx - https://x.com/luxdav
//
// All values below are stock. Remove what you don't change; anything not
// listed keeps its stock value.
//
// Loaded by zzz_luxmod.pk3 on every map load. Server cvars:
//   lux_config   file to load, relative to q3ut4/ (default luxmod.cfg)
//   lux_verbose  1 = list every ignored/unsupported setting
// Server console / rcon:
//   lux_reload         re-read this file now (applies immediately)
//   lux_dump <weapon>  show current values, e.g. lux_dump lr300 / lux_dump player
//   lux_diff           list weapon values that differ from stock
//   lux_throw <player> <he|smoke|hk69>  make a player fire a projectile
//   lux_inv <player>   list a player's weapons and items
//   gh <player|all> <hp>       set health, +N / -N adds (1..100)
//   gw <player|all> <weapons>  give or refill: name (lr300) or letters
//                              (a Knife .. s M4A1, t Glock .. z Magnum)
//   gi <player|all> <items>    give items: name (medkit) or letters
//                              (a Vest, b TacGoggles, c Medkit, d Silencer,
//                               e Laser, f Helmet, g Extra Ammo)
//
// Syntax: "Key: value", "Name {" ... "}", // comments. Unknown settings are
// skipped with a count (lux_verbose 1 lists them).
//
// Weapon settings:
//   Ammo           rounds per magazine
//   Shells         spare magazines (shotguns: spare shells)
//   Reload Time    ms
//   Damage         percent of the hit location values (100 = unchanged);
//                  not for grenades/HK69
//   Knockback      push on hit (bullets; capped at 200)
//   Range          damage falloff distance (stock code: SPAS12 only)
//   Cycle          ms between shots; at weapon level sets every fire mode
//   Speed          projectile speed (grenades, HK69, thrown knife only)
//   Hit Locations  damage in HP per body part, optional "bleed"/"nobleed"
//   Explosive Bullets  splash damage at every bullet impact (bullet weapons;
//                      0 = off; the shooter is never hurt by it)
//   Explosion Radius   radius of those explosions (default 150)
//
// Player { } (whole server):
//   Start Health        health on spawn (1..100)
//   Heal Limit          max health a teammate can bandage you to
//   Heal Limit Medkit   same, when either player has a medkit
//   Heal Step           health per bandage of a teammate
//   Bandage Time        ms a bandage takes (client-predicted, see below)
//   Bandage Time Medkit same, with medkit
//   Bleed Speed         percent; 200 = bleed out twice as fast
//   Fall Injury         percent of the limp you get from falling (0 = none)
//   Vampire             percent of damage you deal that heals you (0 = off)
//   Kill Heal           extra health per kill
//   Vampire Max Health  vampire/kill healing never goes above this
//
// Loadout { } given on every spawn, on top of (or instead of) the chosen gear:
//   Weapons    names or letters, e.g. "deagle he" or "lr300, smoke"
//   Items      e.g. "medkit helmet"
//   Replace    1 = only the loadout, 0 = add it to the player's gear
//   Red { } / Blue { }  per-team loadouts (same keys), override the general one
//   Example:
//     Loadout
//     {
//         Weapons: deagle he
//         Items: medkit
//         Replace: 1
//     }
//
// Damage { } percent of damage per cause (0 = none, 200 = double):
//   Falling, Bleeding, Kick, Goomba, Drowning, Slime, Lava, Crush,
//   Trigger Hurt, Slap. Bleeding is 1 HP per tick, so values below 100
//   act like 0: use Bleed Speed to slow bleeding down instead.
//
// Explosives (HK69, HE Grenade, Smoke Grenade):
//   Fuse             ms until it explodes (cooked grenades keep their head start)
//   Splash Radius    explosion radius
//   Splash Damage    damage at the center, falls off toward the edge
//   Impact Damage    damage of a direct hit
//   Splash Knockback percent of stock push (stock = damage/2, max 200);
//                    also works in Jump mode, where stock has none: grenade jumps
//   Self Damage      percent of splash damage to the thrower (0 = none)
//
// Bullet fire-mode settings (also allowed at weapon level = all modes).
// "Heat" builds up by 30000/Ammo per shot, so Ammo also changes heat per shot.
// The first shot of a burst/spray (heat 0) has no base spread.
//   Spread           base spread, degrees (x0.75 crouched, x0.6 with laser)
//   Moving Spread    extra spread per movement speed
//   Heat Spread      extra spread at full heat, degrees
//   Vertical Spread  upward bullet drift at full heat, degrees
//   Heat Factor      how strongly heat counts toward spread
//   Heat Delay       ms after a shot before heat starts cooling
//   Heat Cooldown    heat removed per ms once cooling (full heat = 30000)
//   Burst Cycle      ms between shots inside a burst
//   Burst Shots      shots per burst (burst modes only)
// Cycle, Burst, Ammo, Reload and Bandage Time are also predicted by the
// unmodified client, so changed values work but sounds/animations may not
// line up perfectly.

"""


# (config key, offset, is_float) - must match weaponFields/modeFields in src/luxmod.c
WEAPON_FIELDS = [("Ammo", 32, False), ("Shells", 28, False), ("Reload Time", 36, False),
                 ("Knockback", 172, True), ("Range", 168, False)]
HITSCAN_FIELDS = [("Cycle", 8, False), ("Spread", 16, True), ("Moving Spread", 20, True),
                  ("Heat Spread", 36, True), ("Vertical Spread", 32, True),
                  ("Heat Factor", 40, True), ("Heat Delay", 24, False),
                  ("Heat Cooldown", 28, False), ("Burst Cycle", 44, False),
                  ("Burst Shots", 48, False)]
PROJECTILE_FIELDS = [("Cycle", 8, False), ("Speed", 36, True)]
OTHER_FIELDS = [("Cycle", 8, False)]

# explosives: (stock fuse ms, splash radius, impact, splash) from the spawn code;
# None = taken from the table's explosive power (impact x50, splash x200)
EXPLOSIVES = {"HK69 40mm": (2500, 300, 90, 180),
              "HE Grenade": (3500, 235, None, None),
              "Smoke Grenade": (15000, 250, None, None)}


def cfg(qvm_path, out_path):
    vm = QVM(qvm_path)
    f = lambda a: struct.unpack_from("<f", vm.mem, a)[0]
    num = lambda v: ("%d" % v) if v == int(v) else ("%g" % round(v, 4))
    val = lambda a, is_float: num(f(a)) if is_float else str(vm.word(a))
    out = [CFG_HEADER,
           "Player\n{\n    Start Health: 100\n    Heal Limit: 50\n    Heal Limit Medkit: 90\n"
           "    Heal Step: 15\n    Bandage Time: 1500\n    Bandage Time Medkit: 750\n"
           "    Bleed Speed: 100\n    Fall Injury: 100\n    Vampire: 0\n    Kill Heal: 0\n"
           "    Vampire Max Health: 100\n}\n\n",
           "Damage\n{\n" + "".join("    %s: 100\n" % c for c in (
               "Falling", "Bleeding", "Kick", "Goomba", "Drowning", "Slime", "Lava",
               "Crush", "Trigger Hurt", "Slap")) + "}\n\n",
           "Weapons\n{\n"]
    for w, e, name in entries(vm):
        if name == "Bomb":
            continue  # plant time is set at runtime from a cvar
        out.append("    %s\n    {\n" % name)
        for key, off, is_float in WEAPON_FIELDS:
            if key == "Range" and name != "Franchi SPAS12":
                continue  # only the SPAS uses damage falloff
            if key == "Knockback" and name in EXPLOSIVES or name == "Flash Grenade" and key == "Knockback":
                continue  # splash knockback is damage/2, see Splash Knockback
            out.append("        %s: %s\n" % (key, val(e + off, is_float)))
        if vm.word(e + 224 + 4) not in PROJECTILE_TYPES:
            out.append("        Damage: 100\n")
        if name in EXPLOSIVES:
            fuse, radius, impact, splash = EXPLOSIVES[name]
            power = f(e + HIT_OFF)
            out.append("        Fuse: %d\n" % fuse)
            out.append("        Splash Radius: %d\n" % radius)
            out.append("        Splash Damage: %d\n" % (splash if splash is not None else int(power * 200)))
            out.append("        Impact Damage: %d\n" % (impact if impact is not None else int(power * 50)))
            out.append("        Splash Knockback: 100\n")
            out.append("        Self Damage: 100\n")
        for m in range(4):
            b = e + 224 + 52 * m
            mn = vm.string_at(vm.word(b))
            if not mn:
                continue
            kind = vm.word(b + 4)
            fields = (HITSCAN_FIELDS if kind in (0, 1) else
                      PROJECTILE_FIELDS if kind in PROJECTILE_TYPES else OTHER_FIELDS)
            out.append("        %s\n        {\n" % mn)
            for key, off, is_float in fields:
                if key.startswith("Burst") and not vm.word(b + 48):
                    continue  # not a burst mode
                out.append("            %s: %s\n" % (key, val(b + off, is_float)))
            out.append("        }\n")
        out.append("        Hit Locations\n        {\n")
        for i, loc in enumerate(HITLOCS[1:], 1):
            a = e + HIT_OFF + 8 * i
            out.append("            %s: %s %s\n" % (
                loc, num(round(f(a) * 100, 2)), "bleed" if vm.word(a + 4) else "nobleed"))
        out.append("        }\n    }\n")
    out.append("}\n")
    open(out_path, "w").write("".join(out))
    print("wrote %s" % out_path)


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "dump":
        dump(a[1], a[2])
    elif len(a) == 3 and a[0] == "cfg":
        cfg(a[1], a[2])
    elif len(a) in (4, 6) and a[0] == "apply":
        apply(a[1], a[2], a[3], a[5] if len(a) == 6 and a[4] == "--pk3" else None)
    else:
        sys.exit(__doc__)
