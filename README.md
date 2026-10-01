# luxmod

Server-side mod for **Urban Terror 4.3.4** that makes weapons, explosives and
player values configurable from a text file, live, without a modified client.

by **LuxXx** - [github.com/LuxXx](https://github.com/LuxXx) - [x.com/luxdav](https://x.com/luxdav)

- Per weapon: damage per body part and bleeding, magazine size, spare
  magazines, reload time, fire rate, spread and recoil, burst, knockback
- Explosives (HE, HK69, smoke): fuse, splash radius and damage, impact damage,
  splash knockback and self damage - including grenade jumps in Jump mode
- Player: start health, teammate healing, bandage time, bleed speed, fall
  injury, and damage per cause (falling, kick, bleeding, drowning, ...)
- Reloaded on every map, or instantly with `lux_reload`
- Admin commands to give health, weapons and items

Only the server needs it. Players connect with the normal Urban Terror client.

## How it works

The Urban Terror game code (`qagame.qvm`) is closed source. luxmod is written
in C, compiled with the Quake 3 bytecode compiler, and appended to the stock
`qagame.qvm`: a few calls inside the game are redirected to luxmod, which
then runs the original code and applies your settings. Nothing in the stock
code moves, so the game behaves exactly as stock wherever the config doesn't
say otherwise.

This repository contains **no Urban Terror files**. You build the mod from
your own Urban Terror 4.3.4 install.

## Build

Requirements: Linux, Python 3, gcc, make, git, patch, and an Urban Terror
**4.3.4** install (the build refuses any other `qagame.qvm`).

```sh
scripts/setup_toolchain.sh          # downloads ioUrbanTerror, builds q3lcc + patched q3asm
python3 tools/build_luxmod.py --urt /path/to/UrbanTerror43/q3ut4 --install
```

`--install` copies `build/zzz_luxmod.pk3` into the `q3ut4` folder, and
`config/luxmod.cfg` too if there is no `luxmod.cfg` there yet (an existing one
is never overwritten). Without `--install`, copy the two files yourself.

Start the server as usual, for example:

```sh
./Quake3-UrT-Ded.x86_64 +set dedicated 1 +map ut4_turnpike
```

The console shows `luxmod: loaded luxmod.cfg: ... applied, ... errors`.
Remove `zzz_luxmod.pk3` to go back to stock.

## Config

`config/luxmod.cfg` lists every setting with its stock value and explains each
one at the top. Change what you want and delete the rest: anything not listed
keeps its stock value. Mistakes are reported with their line number.

```
Player
{
    Start Health: 100
}

Weapons
{
    ZM LR300
    {
        Ammo: 30
        Automatic
        {
            Cycle: 120
            Spread: 0.5
        }
        Hit Locations
        {
            Torso: 44 bleed
        }
    }
    HE Grenade
    {
        Splash Knockback: 300
        Self Damage: 0
    }
}
```

## Commands

Server console or rcon:

| Command | |
|---|---|
| `lux_reload` | re-read the config now |
| `lux_dump <weapon\|player>` | show current values, e.g. `lux_dump lr300` |
| `lux_diff` | list weapon values that differ from stock |
| `lux_throw <player> <he\|smoke\|hk69>` | make a player fire a projectile |
| `gh <player\|all> <hp>` | set health, `+N` / `-N` adds |
| `gw <player\|all> <weapons>` | give or refill weapons: name (`lr300`) or letters (`a` Knife ... `s` M4A1, `t` Glock ... `z` Magnum) |
| `gi <player\|all> <items>` | give items: name (`medkit`) or letters (`a` Vest, `b` TacGoggles, `c` Medkit, `d` Silencer, `e` Laser, `f` Helmet, `g` Extra Ammo) |

Cvars: `lux_config` (file to load, default `luxmod.cfg`), `lux_verbose`
(`1` lists ignored settings, `2` also logs explosives).

## Limitations

- Urban Terror **4.3.4** only: everything is tied to that exact `qagame.qvm`.
- Fire rate (`Cycle`), burst, ammo, reload and bandage time are also predicted
  by the unmodified client. Changed values work, but sounds and animations may
  not line up perfectly. Damage, spread, knockback and everything explosive
  are decided by the server only.
- Stamina and movement physics are not configurable: the client runs the same
  code, so changing it on the server alone causes rubber-banding.

## Tools

- `tools/build_luxmod.py` - builds the pk3 (see above)
- `tools/qvmdis.py` - QVM disassembler with string and syscall annotations
- `tools/qvmdecomp.py` - turns a QVM function back into C-like statements
- `tools/utweapons.py cfg <qagame.qvm> <out.cfg>` - regenerates the stock config

## Credits and licenses

- luxmod by LuxXx.
- Urban Terror is (c) FrozenSand Games Limited. luxmod is not affiliated with
  or endorsed by FrozenSand, and contains none of their files.
- The QVM toolchain comes from [ioUrbanTerror](https://github.com/urbanterror/ioUrbanTerror):
  q3asm (GPL-2.0, id Software; our change is `toolchain/q3asm-graft.patch`)
  and lcc (lcc license, Fraser & Hanson).
