"""Generate src/ootgz/data.c (the native menu's tables) from this folder's JSON files.

    python tools/gendata.py

Run it after changing field_hashes.json, extra_fields.json, story.json or warps.json and commit
the result: the native build itself doesn't need Python. The lists below mirror ootmenu.pyw.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

# --- same lists as ootmenu.pyw -----------------------------------------------------------------
ITEM_SCREEN = ([("acquired", "Deku sticks"), ("owned", "Fairy Slingshot"), ("owned", "Boomerang"),
                ("acquired", "Deku nuts"), ("acquired", "Bombs"), ("acquired", "Bombchus"), ("owned", "Hammer"),
                ("owned", "Hookshot"), ("owned", "Longshot"), ("owned", "Fairy Bow"), ("owned", "Arrow Fire"),
                ("owned", "Arrow Ice"), ("owned", "Arrow Light"), ("acquired", "Magic Bean"),
                ("owned", "Din’s Fire"), ("owned", "Farore’s Wind"), ("owned", "Nayru’s Love")]
               + [("trade", "child"), ("trade", "adult")]
               + [("owned", "Lens of Truth"), ("owned", "Gerudo membership card"), ("ocarina", "ocarina")])
GEAR = ["Kokiri Sword", "Master Sword", "Giant’s Knife", "Broken Giant’s Knife", "Biggoron’s Sword",
        "Deku Shield", "Hylian Shield", "Mirror Shield", "Goron Tunic", "Zora Tunic", "Iron Boots", "Hover Boots",
        # owned flags only (inventory > collection); giving one for real is the ocarina switch
        "Fairy Ocarina", "Ocarina of Time"]
TRADE_ITEMS = [("weird egg", "Item_WeirdEgg", "WeirdEgg"), ("cucco", "Item_AlarmCucco", "AlarmCucco"),
               ("zelda's letter", "Item_ZeldaLetter", "ZeldaLetter"), ("pocket egg", "Item_PocketEgg", "PocketEgg"),
               ("pocket cucco", "Item_PocketCucco", "PocketCucco"), ("cojiro", "Item_CojiroCucco", "CojiroCucco"),
               ("odd mushroom", "Item_OddMushroom", "OddMushroom"), ("odd potion", "Item_OddPowder", "OddPowder"),
               ("poacher's saw", "Item_PoacherSaw", "PoacherSaw"), ("broken sword", "Item_BrokenSword", "BrokenSword"),
               ("prescription", "Item_Prescription", "Prescription"),
               ("eyeball frog", "Item_EyeballFrog", "EyeballFrog"), ("eye drops", "Item_EyeDrop", "EyeDrop"),
               ("claim check", "Item_ClaimCheck", "ClaimCheck")]
MASKS = [("bunny hood" if m == "Bunny" else f"{m.lower()} mask", f"Armor_Child_Mask_{m}", f"{m}Mask")
         for m in ("Keaton", "Skull", "Spooky", "Bunny", "Goron", "Zora", "Gerudo", "Truth")]
BUTTON_ITEM = {"Fairy Slingshot": "SlingShot", "Fairy Bow": "Bow", "Arrow Fire": "FireArrow",
               "Arrow Ice": "IceArrow", "Arrow Light": "LightArrow", "Boomerang": "Boomerang",
               "Hookshot": "Hookshot", "Longshot": "Longshot", "Hammer": "Hammer",
               "Lens of Truth": "GlassTruth", "Din’s Fire": "DinFire", "Farore’s Wind": "FaroreWind",
               "Nayru’s Love": "NayruLove", "Iron Boots": "HeavyBoot", "Hover Boots": "HoverBoot"}
GD_STR = {"sword": "GD_STR_SWORD", "shield": "GD_STR_SHIELD", "tunic": "GD_STR_TUNIC", "boots": "GD_STR_BOOTS"}
WORN_GEAR = {"Kokiri Sword": ("sword", "Weapon_Sword_Kokiri"), "Master Sword": ("sword", "Weapon_Sword_Master"),
             "Giant’s Knife": ("sword", "Weapon_Sword_Giant"),
             "Broken Giant’s Knife": ("sword", "Weapon_Sword_BrokenGiant"),
             "Biggoron’s Sword": ("sword", "Weapon_Sword_Biggoron"), "Deku Shield": ("shield", "Weapon_Shield_Deku"),
             "Hylian Shield": ("shield", "Weapon_Shield_Hylian"), "Mirror Shield": ("shield", "Weapon_Shield_Mirror"),
             "Goron Tunic": ("tunic", "Armor_Adult_Goron"), "Zora Tunic": ("tunic", "Armor_Adult_Zora"),
             "Iron Boots": ("boots", "Armor_Adult_HeavyBoot"), "Hover Boots": ("boots", "Armor_Adult_HoverBoot")}
AMMO = [("deku sticks", "Deku sticks", "Deku Stick capacity", "Deku sticks"),
        ("deku nuts", "Deku nuts", "Deku Nut capacity", "Deku nuts"),
        ("deku seeds", "Deku seeds", "Deku Seed capacity", "Deku Seed"),
        ("bombs", "Bombs", "Bomb capacity", "Bombs"), ("arrows", "Arrows", "Arrow capacity", "Arrows"),
        ("bombchus", "Bombchus", 50, "Bombchus"), ("magic beans", "Magic Bean", 10, "Magic Bean")]
SONGS = ["Zelda's Lullaby", "Epona's Song", "Saria's Song", "Sun's Song", "Song of Time", "Song of Storms",
         "Scarecrow's Song", "Minuet of Forest", "Bolero of Fire", "Serenade of Water", "Requiem of Spirit",
         "Nocturne of Shadow", "Prelude of Light"]
QUEST_ITEMS = ["Kokiri's Emerald", "Goron's Ruby", "Zora's Sapphire", "Forest Medallion", "Fire Medallion",
               "Water Medallion", "Spirit Medallion", "Shadow Medallion", "Light Medallion"]
DUNGEON_LIST = [("Inside The Deku Tree", "deku tree"), ("Dodongo Cavern", "dodongo's cavern"),
                ("Inside Jabu Jabu Belly", "jabu-jabu"), ("Forest Temple", "forest temple"),
                ("Fire Temple", "fire temple"), ("Water Temple", "water temple"),
                ("Shadow Temple", "shadow temple"), ("Spirit Temple", "spirit temple"),
                ("Bottom Of The Well", "bottom of well"), ("Ice Cavern", "ice cavern"),
                ("Gerudo Training Ground", "gerudo training"), ("Gerudo Fortress", "gerudo fortress"),
                ("Inside Ganon Castle", "ganon's castle"), ("Final Battle Field", "final battle")]
DUNGEON_ITEMS = [("Map", "map", "Dungeon Map"), ("Compass", "compass", "Dungeon Compass"),
                 ("Boss key", "boss key", "Dungeon Boss Key")]
BOTTLE_ACQ = {"Red Potion": "Bottle Red Potion", "Green Potion": "Bottle Green Potion",
              "Blue Potion": "Bottle Blue Potion", "Blue Fire": "Bottle Blue Fire", "Fish": "Bottle Fish",
              "Letter": "Bottle Letter", "Big Poe": "Bottle Big Poe", "Poe": "Bottle Poe", "Bug": "Bottle Bug",
              "Fairy": "Bottle Fairy", "Max Milk": "Bottle Milk", "Half Milk": "Bottle Half Milk"}
DUNGEON_KEYS = ["Forest Temple", "Fire Temple", "Water Temple", "Shadow Temple", "Spirit Temple",
                "Bottom Of The Well", "Gerudo Training Ground", "Inside Ganon Castle"]
QUEST_NAMES = {"Step_Get_StoneOfForest": "deku tree", "Step_Get_StoneOfFire": "dodongo's cavern",
               "Step_Get_StoneOfWater": "jabu-jabu", "Step_SageOfForest": "forest temple",
               "Step_SageOfFire": "fire temple", "Step_SageOfWater": "water temple",
               "Step_SageOfShadow": "shadow temple", "Step_SageOfSpirit": "spirit temple",
               "Step_EscapeFromCrumblingGanonCastle": "castle escape", "Step_GatheringTokenGold": "skulltulas",
               "Step_SubEvent_GetSwordBigGoron": "biggoron sword", "Step_SubEvent_AllBossBattle": "boss rush",
               "Step_Goto_HyruleCastle": "zelda / castle", "Step_Meet_DekuTree": "kokiri sword",
               "Step_Minigame_SkullKidSession": "skull kid song"}
GATED_SPAWNS = {"MountainTrail_DodongoCavernSide": "boulder bombed",
                "GoronCity_LostWoodSide_before": "lost woods wall bombed",
                "GoronCity_LostWoodSide_after": "lost woods wall bombed",
                "LostWood_GoronCitySide_before": "goron city wall bombed",
                "LostWood_GoronCitySide_after": "goron city wall bombed"}
DUNGEONS = {"InsideTheDekuTree", "DodongoCavern", "InsideJabuJabuBelly", "ForestTemple", "FireTemple",
            "WaterTemple", "ShadowTemple", "SpiritTemple", "BottomOfTheWell", "IceCavern",
            "GerudoTrainingGround", "InsideGanonCastle", "CrumblingGanonCastle"}
NAME_FIXES = {"hounted": "haunted", "musuem": "museum", "caslte": "castle", "wateland": "wasteland",
              "caven": "cavern"}
CATEGORIES = ["overworld (child)", "overworld (adult)", "dungeons", "boss & battle rooms", "other"]


def pretty(name):
    parts = name.split("_")
    tags = [p.lower() for p in parts[1:] if p in ("Child", "Adult", "Alternative")]
    words = " ".join(p for p in parts if p.lower() not in tags)
    words = re.sub(r"(?<=[a-z])(?=[A-Z0-9])|(?<=[A-Z])(?=[A-Z][a-z])", " ", words).lower()
    for typo, fixed in NAME_FIXES.items():
        words = words.replace(typo, fixed)
    return words + (f" ({' '.join(tags)})" if tags else "")


def map_label(name):
    """pretty() without the (child) / (adult) tag: the warps menu's category (breadcrumb) says it."""
    return re.sub(r" \((child|adult)\)$", "", re.sub(r" \((child|adult) ", " (", pretty(name)))


def quest_label(name):
    if name in QUEST_NAMES:
        return QUEST_NAMES[name]
    n = re.sub(r"^Step_(Get_|Goto_|SubEvent_|Mini[Gg]ame_)?", "", name).replace("_", " ")
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", n).lower()


def label_of(title):
    return title.lower().replace("’", "'")


def category(name):
    if name in DUNGEONS:
        return 2
    if name.endswith("Stage") or name in ("BossRushIntervalRoom", "FinalBattleField"):
        return 3
    if name.endswith("_Child"):
        return 0
    if "_Adult" in name or name in ("HyruleCastle", "GanonCastle"):
        return 1
    return 4


def c(s):
    return "NULL" if s is None else json.dumps(s.replace("’", "'"))


def fl(v):
    t = f"{v:g}"
    return t + ("" if any(ch in t for ch in ".en") else ".0") + "f"


def main():
    fields = json.loads((ROOT / "field_hashes.json").read_text(encoding="utf-8"))
    extra = json.loads((ROOT / "extra_fields.json").read_text(encoding="utf-8"))
    story = json.loads((ROOT / "story.json").read_text(encoding="utf-8"))
    warps = json.loads((ROOT / "warps.json").read_text(encoding="utf-8"))
    starters = json.loads((ROOT / "spawn_starters.json").read_text(encoding="utf-8"))
    F = {m["title"]: (int(h, 16), m.get("type", "Int")) for h, m in fields.items()
         if m.get("group") not in ("Acquisition history", "Assignment order")}
    A = {m["title"]: int(h, 16) for h, m in fields.items() if m.get("group") == "Acquisition history"}
    TYPES = {"Int": "FT_INT", "Bool": "FT_BOOL", "Enum": "FT_ENUM", "Float": "FT_FLOAT"}

    def fld(h, t):
        return f"{{ {h:#010x}, {TYPES[t]} }}"

    def xf(key):
        return fld(int(extra[key]["hash"], 16), extra[key]["type"]) if key in extra else "{ 0, FT_NONE }"

    def acq(title):
        return f"{A.get(title, 0):#010x}"

    out = ["/* Generated by tools/gendata.py from field_hashes.json, extra_fields.json, story.json and",
           " * warps.json. Don't edit by hand. */", '#include "data.h"', '#include "gamedata.h"', "",
           "#define NULL ((void *)0)", ""]
    w = out.append

    def inv_item(title, kind):
        h, t = F.get(title, (0, "Int"))
        slot, worn = WORN_GEAR.get(title, (None, None))
        return (f"  {{ {c(label_of(title))}, {kind}, {fld(h, t)}, {acq(title)}, {c(BUTTON_ITEM.get(title))}, "
                f"{GD_STR[slot] if slot else -1}, {c(worn)} }},")

    w("const inv_item_t data_items[] = {")
    for kind, t in ITEM_SCREEN:
        if kind == "trade":
            w(f"  {{ {c(t + ' trade')}, {'IK_TRADE_CHILD' if t == 'child' else 'IK_TRADE_ADULT'}, {{ 0, FT_NONE }}, 0, NULL, -1, NULL }},")
        elif kind == "ocarina":
            w('  { "ocarina", IK_OCARINA, { 0, FT_NONE }, 0, NULL, -1, NULL },')
        elif kind == "acquired":
            w(f"  {{ {c(label_of(t))}, IK_ACQUIRED, {{ 0, FT_NONE }}, {acq(t)}, NULL, -1, NULL }},")
        else:
            w(inv_item(t, "IK_OWNED"))
    w("};\nconst int data_n_items = sizeof(data_items) / sizeof(*data_items);\n")
    w("const inv_item_t data_gear[] = {")
    for t in GEAR:
        w(inv_item(t, "IK_OWNED"))
    w("};\nconst int data_n_gear = sizeof(data_gear) / sizeof(*data_gear);\n")

    def one_table(name, rows):
        w(f"const pouch_one_t {name}[] = {{")
        for lbl, item, button in rows:
            w(f"  {{ {c(lbl)}, {c(item)}, {c(button)} }},")
        w(f"}};\nconst int {name.replace('data_', 'data_n_')} = sizeof({name}) / sizeof(*{name});\n")
    one_table("data_masks", MASKS)
    one_table("data_trade_child", TRADE_ITEMS[:3])
    one_table("data_trade_adult", TRADE_ITEMS[3:])

    w("const ammo_t data_ammo[] = {")
    for lbl, title, cap, acq_title in AMMO:
        capf = F[cap][0] if isinstance(cap, str) else 0
        fixed = cap if isinstance(cap, int) else 0
        w(f"  {{ {c(lbl)}, {F[title][0]:#010x}, {capf:#010x}, {fixed}, {acq(acq_title)} }},")
    w("};\nconst int data_n_ammo = sizeof(data_ammo) / sizeof(*data_ammo);\n")

    def flag(lbl, keys, acq_title=None):
        f = [xf(k) for k in keys] + ["{ 0, FT_NONE }"] * (2 - len(keys))
        return f"{{ {c(lbl)}, {{ {f[0]}, {f[1]} }}, {acq(acq_title) if acq_title else 0} }}"

    w("const flag_t data_quest_items[] = {")
    for q in QUEST_ITEMS:
        w("  " + flag(label_of(q), [f"quests/Quest items/{q}", f"inventory/Items/{q}"]) + ",")
    w("};\nconst int data_n_quest_items = sizeof(data_quest_items) / sizeof(*data_quest_items);\n")
    w("const flag_t data_songs[] = {")
    for s in SONGS:
        w("  " + flag(label_of(s), [f"quests/Songs/{s}"]) + ",")
    w("};\nconst int data_n_songs = sizeof(data_songs) / sizeof(*data_songs);\n")
    w("const dungeon_t data_dungeons[] = {")
    for d, lbl in DUNGEON_LIST:
        items = ", ".join(flag(l, [f"dungeons/{d}/{t}"], a) for t, l, a in DUNGEON_ITEMS)
        w(f"  {{ {c(lbl)}, {{ {items} }} }},")
    w("};\nconst int data_n_dungeons = sizeof(data_dungeons) / sizeof(*data_dungeons);\n")
    w("const number_t data_small_keys[] = {")
    for d in DUNGEON_KEYS:
        w(f"  {{ {c(label_of(d))}, {F[f'{d} small keys'][0]:#010x} }},")
    w("};\nconst int data_n_small_keys = sizeof(data_small_keys) / sizeof(*data_small_keys);\n")
    w(f'const number_t data_skulltulas = {{ "skulltulas", {F["Gold Skulltula tokens"][0]:#010x} }};')
    w("const flag_t data_double_defense = " + flag("double defense", ["link/Abilities/Double defense"]) + ";")
    w("const flag_t data_agony = " + flag("stone of agony", ["equipment/Effects & appearance/Stone of Agony equipped"],
                                          "Agony Stone") + ";\n")

    w("const bottle_t data_bottles[5] = {")
    for b in "ABCDE":
        w(f"  {{ {xf(f'inventory/Items/Bottle {b}')}, {int(extra[f'inventory/Bottles/Bottle {b} available']['hash'], 16):#010x}, "
          f"{int(extra[f'inventory/Bottles/Bottle {b} contents']['hash'], 16):#010x} }},")
    w("};")
    choices = extra["inventory/Bottles/Bottle A contents"]["choices"]
    w("const char *const data_bottle_names[] = { " + ", ".join(c(l.lower()) for _, l in choices) + " };")
    w("const uint32_t data_bottle_values[] = { " + ", ".join(f"{v:#010x}" for v, _ in choices) + " };")
    w("const uint32_t data_bottle_acquired[] = { " + ", ".join(acq(BOTTLE_ACQ.get(l, "")) for _, l in choices) + " };")
    w(f"const int data_n_bottle_contents = {len(choices)};")
    w(f"const uint32_t data_acquired_bottle = {acq('Bottle')};\n")

    st = extra["link/Abilities/Strength"]
    w(f"const field_t data_strength = {xf('link/Abilities/Strength')};")
    w("const uint32_t data_strength_values[4] = { " + ", ".join(f"{v:#010x}" for v, _ in st["choices"]) + " };")
    w(f"const field_t data_diving_depth = {xf('link/Abilities/Diving depth')};")
    w("const float data_scale_depths[3] = { 3.0f, 6.0f, 9.0f };\n")

    quests = sorted(story["quests"].items(), key=lambda x: quest_label(x[1]["name"]))
    for i, (h, q) in enumerate(quests):
        w(f"static const char *const stages_{i}[] = {{ " + ", ".join(c(s) for s in q["stages"]) + " };")
    w("const quest_t data_quests[] = {")
    for i, (h, q) in enumerate(quests):
        w(f"  {{ {c(quest_label(q['name']))}, {int(h, 16):#010x}, {len(q['stages'])}, stages_{i} }},")
    w("};\nconst int data_n_quests = sizeof(data_quests) / sizeof(*data_quests);\n")
    guides = story["action_guides"]
    w("const uint32_t data_action_guides[] = { " + ", ".join(f"{int(v, 16):#010x}" for v in guides.values()) + " };")
    w("const int data_n_action_guides = sizeof(data_action_guides) / sizeof(*data_action_guides);")
    w(f"const uint32_t data_action_guide_navi = {int(guides['Navi'], 16):#010x};\n")

    w("const char *const data_warp_categories[WC_COUNT] = { " + ", ".join(c(x) for x in CATEGORIES) + " };")
    spawns, maps = [], []
    for cat in range(len(CATEGORIES)):
        for name in sorted((n for n in warps["spawns"] if category(n) == cat), key=pretty):
            first = len(spawns)
            for pos, event, src in warps["spawns"][name]:
                label = pretty(pos)
                if pos in GATED_SPAWNS:
                    label = f"{label} [needs {GATED_SPAWNS[pos]}]"
                else:
                    origin = pretty(src[5:]).split(" (")[0] if src.startswith("from ") else ""
                    if origin and origin != label:
                        label = f"{label} (from {origin})"
                spawns.append((label, pos, event, int(bool(starters.get(name, {}).get(pos)))))
            maps.append((name, map_label(name), cat, int(name in DUNGEONS), first, len(spawns) - first))
    w("const warp_map_t data_warp_maps[] = {")
    for name, lbl, cat, dun, first, n in maps:
        w(f"  {{ {c(name)}, {c(lbl)}, {cat}, {dun}, {first}, {n} }},")
    w("};\nconst int data_n_warp_maps = sizeof(data_warp_maps) / sizeof(*data_warp_maps);\n")
    w("const spawn_t data_spawns[] = {")
    for lbl, pos, event, starter in spawns:
        w(f"  {{ {c(lbl)}, {c(pos)}, {c(event)}, {starter} }},")
    w("};\nconst int data_n_spawns = sizeof(data_spawns) / sizeof(*data_spawns);")

    # air walls (airwalls.json, from tools/airwalls.py): per map, walls with their shapes
    aw = json.loads((ROOT / "airwalls.json").read_text(encoding="utf-8"))["maps"]
    blocks = {"HitOnlyPlayer", "Default", None}  # airwall.py PLAYER_MASKS
    w("")
    w("const airshape_t data_airshapes[] = {")
    walls, amaps, nshape = [], [], 0
    for name in sorted(aw):
        first = len(walls)
        for d in aw[name]:
            f0 = nshape
            for sh in d["shapes"]:
                if sh["type"] == "box":
                    a3, b3, r = sh["center"], sh["half"], 0.0
                else:
                    a3, b3, r = sh["a"], sh["b"], sh["radius"]
                w(f"  {{ {0 if sh['type'] == 'box' else 1}, {{ {fl(a3[0])}, {fl(a3[1])}, {fl(a3[2])} }}, "
                  f"{{ {fl(b3[0])}, {fl(b3[1])}, {fl(b3[2])} }}, {fl(r)} }},")
                nshape += 1
            walls.append((d["t"], d["r"], d["s"], f0, nshape - f0, 0 if d.get("mask") in blocks else 1))
        amaps.append((name, first, len(walls) - first))
    w("};")
    w("const airwall_t data_airwalls[] = {")
    for t, r, sc, f0, n, kind in walls:
        w(f"  {{ {{ {fl(t[0])}, {fl(t[1])}, {fl(t[2])} }}, {{ {fl(r[0])}, {fl(r[1])}, {fl(r[2])} }}, "
          f"{{ {fl(sc[0])}, {fl(sc[1])}, {fl(sc[2])} }}, {f0}, {n}, {kind} }},")
    w("};")
    w("const airmap_t data_airmaps[] = {")
    for name, first, n in amaps:
        w(f"  {{ {c(name)}, {first}, {n} }},")
    w("};\nconst int data_n_airmaps = sizeof(data_airmaps) / sizeof(*data_airmaps);")

    missing = [t for k, t in ITEM_SCREEN if k in ("owned",) and t not in F] + [t for t in GEAR if t not in F]
    if missing:
        sys.exit(f"missing fields: {missing}")
    (ROOT / "src/ootgz/data.c").write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"src/ootgz/data.c: {len(maps)} maps, {len(spawns)} spawns, {len(quests)} quests, "
          f"{len(walls)} air walls in {len(amaps)} maps")


if __name__ == "__main__":
    main()
