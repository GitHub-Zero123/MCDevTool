# -*- encoding: utf-8 -*-
__all__ = [
    "run_with_config"
]


import json, os, shutil, struct, sys


class NbtTagType:
    END         = 0x0
    BYTE        = 0x1
    SHORT       = 0x2
    INT         = 0x3
    LONG        = 0x4
    FLOAT       = 0x5
    DOUBLE      = 0x6
    BYTE_ARRAY  = 0x7
    STRING      = 0x8
    LIST        = 0x9
    COMPOUND    = 0xA
    INT_ARRAY   = 0xB
    LONG_ARRAY  = 0xC


class NbtObject(object):
    def __repr__(self):
        # type: () -> str
        raise NotImplementedError

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        raise NotImplementedError

    @property
    def binary(self):
        # type: () -> str
        raise NotImplementedError

    def deserialize(self):
        # type: () -> str
        return chr(self.tag_type) + self.binary


class NbtEnd(NbtObject):
    def __repr__(self):
        # type: () -> str
        return "NbtEnd()"

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.END

    @property
    def binary(self):
        # type: () -> str
        return ""


class NbtByte(NbtObject):
    def __init__(self, value):
        # type: (int) -> None
        if not isinstance(value, int):
            raise TypeError
        if not (-128 <= value <= 127):
            raise ValueError
        self.__value = value

    def __repr__(self):
        # type: () -> str
        return "NbtByte({value})".format(value=self.__value)

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.BYTE

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("b", self.__value)


class NbtShort(NbtObject):
    def __init__(self, value):
        # type: (int) -> None
        if not isinstance(value, int):
            raise TypeError
        if not (-32768 <= value <= 32767):
            raise ValueError
        self.__value = value

    def __repr__(self):
        # type: () -> str
        return "NbtShort({value})".format(value=self.__value)

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.SHORT

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("<h", self.__value)


class NbtInt(NbtObject):
    def __init__(self, value):
        # type: (int) -> None
        if not isinstance(value, int):
            raise TypeError
        if not (-2147483648 <= value <= 2147483647):
            raise ValueError
        self.__value = value

    def __repr__(self):
        # type: () -> str
        return "NbtInt({value})".format(value=self.__value)

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.INT

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("<i", self.__value)


class NbtLong(NbtObject):
    def __init__(self, value):
        # type: (long) -> None
        if not isinstance(value, (int, long)):
            raise TypeError
        if not (-9223372036854775808 <= value <= 9223372036854775807):
            raise ValueError
        self.__value = value

    def __repr__(self):
        # type: () -> str
        return "NbtLong({value})".format(value=self.__value)

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.LONG

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("<q", self.__value)


class NbtFloat(NbtObject):
    def __init__(self, value):
        # type: (float) -> None
        if not isinstance(value, (int, float)):
            raise TypeError
        self.__value = float(value)

    def __repr__(self):
        # type: () -> str
        return "NbtFloat({value})".format(value=self.__value)

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.FLOAT

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("<f", self.__value)


class NbtDouble(NbtObject):
    def __init__(self, value):
        # type: (float) -> None
        if not isinstance(value, (int, float)):
            raise TypeError
        self.__value = float(value)

    def __repr__(self):
        # type: () -> str
        return "NbtDouble({value})".format(value=self.__value)

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.DOUBLE

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("<d", self.__value)


class NbtByteArray(NbtObject):
    def __init__(self, value):
        # type: (list) -> None
        if not isinstance(value, list):
            raise TypeError
        for byte in value:
            if not isinstance(byte, int):
                raise TypeError
            if not (0 <= byte <= 255):
                raise ValueError
        self.__value = value

    def __repr__(self):
        # type: () -> str
        return "NbtByteArray([{value}])".format(value=", ".join(self.__value))

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.BYTE_ARRAY

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("<i", len(self.__value)) + "".join([struct.pack("B", x & 0xFF) for x in self.__value])


class NbtString(NbtObject):
    def __init__(self, value):
        # type: (str) -> None
        if not isinstance(value, basestring):
            raise TypeError
        self.__value = unicode(value) if isinstance(value, str) else value

    def __repr__(self):
        # type: () -> str
        return "NbtString(\"{value}\")".format(value=self.__value)

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.STRING

    @property
    def binary(self):
        # type: () -> str
        string = self.__value.encode("utf-8")
        return struct.pack("<h", len(string)) + string


class NbtList(NbtObject):
    def __init__(self, value):
        # type: (list) -> None
        if not isinstance(value, list):
            raise TypeError
        self.__type = None
        for element in value:
            if not isinstance(element, NbtObject):
                raise TypeError
            if self.__type is None:
                self.__type = element.tag_type
            if element.tag_type != self.__type:
                raise TypeError
        self.__value = value

    def __repr__(self):
        # type: () -> str
        return "NbtList([{value}])".format(value=", ".join(repr(element) for element in self.__value))

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.LIST

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("<i", len(self.__value)) + "".join([element.binary for element in self.__value])


class NbtCompound(NbtObject):
    def __init__(self, value):
        # type: (dict) -> None
        if not isinstance(value, dict):
            raise TypeError
        for key, _value in value.iteritems():
            if not isinstance(key, NbtString):
                raise TypeError
            if not isinstance(_value, NbtObject):
                raise TypeError
        self.__value = value

    def __repr__(self):
        # type: () -> str
        return "NbtCompound({value})".format(value=str(self.__value))

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.COMPOUND

    @property
    def binary(self):
        # type: () -> str
        content = ""
        for key, value in self.__value.iteritems():
            content += chr(value.tag_type)
            content += key.binary
            content += value.binary
        return content + chr(NbtTagType.END)


class NbtIntArray(NbtObject):
    def __init__(self, value):
        # type: (list) -> None
        if not isinstance(value, list):
            raise TypeError
        for integer in value:
            if not isinstance(integer, int):
                raise TypeError
            if not (-2147483648 <= integer <= 2147483647):
                raise ValueError
        self.__value = value

    def __repr__(self):
        # type: () -> str
        return "NbtIntArray([{value}])".format(value=", ".join(self.__value))

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.INT_ARRAY

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("<i", len(self.__value)) + "".join([struct.pack("<i", integer) for integer in self.__value])


class NbtLongArray(NbtObject):
    def __init__(self, value):
        # type: (list) -> None
        if not isinstance(value, list):
            raise TypeError
        for integer in value:
            if not isinstance(integer, (int, long)):
                raise TypeError
            if not (-9223372036854775808 <= integer <= 9223372036854775807):
                raise ValueError
        self.__value = value

    def __repr__(self):
        # type: () -> str
        return "NbtLongArray([{value}])".format(value=", ".join(self.__value))

    @property
    def tag_type(self):
        # type: () -> NbtTagType
        return NbtTagType.LONG_ARRAY

    @property
    def binary(self):
        # type: () -> str
        return struct.pack("<i", len(self.__value)) + "".join([struct.pack("<q", integer) for integer in self.__value])


def check_python_version():
    # type: () -> None
    if sys.version_info.major != 2:
        raise RuntimeError("Python 2 is required.")


def read_config(config_file_path):
    # type: (str) -> dict
    with open(config_file_path, "r") as istream:
        return json.load(istream)


def check_required_keys(data, required_keys):
    # type: (dict, tuple) -> None
    for keys in required_keys:
        current, depth = data, len(keys)
        for index in xrange(depth):
            current = current[keys[index]]


def _recursion_fill_default_items(data, default_items):
    # type: (dict, dict) -> None
    for key, default_value in default_items.iteritems():
        if key not in data:
            data[key] = default_value
        elif isinstance(data[key], dict) and isinstance(default_value, dict):
            _recursion_fill_default_items(data[key], default_value)


def fill_default_items(data, default_items):
    # type: (dict, dict) -> None
    _recursion_fill_default_items(data, default_items)


def _get_app_data_path():
    # type: () -> str
    return os.getenv("APPDATA")


def _get_minecraft_data_path():
    # type: () -> str
    return os.path.join(_get_app_data_path(), "MinecraftPE_Netease")


def get_minecraft_worlds_path():
    # type: () -> str
    return os.path.join(_get_minecraft_data_path(), "minecraftWorlds")


def _make_dir(dir_path, override):
    # type: (str, bool) -> None
    if os.path.exists(dir_path):
        if not override:
            return
        if os.path.isfile(dir_path):
            os.remove(dir_path)
        else:
            shutil.rmtree(dir_path)
    os.makedirs(dir_path)


def build_world(start_info, worlds_path):
    # type: (dict, str) -> str
    level_id = start_info["level_id"]
    override_world = start_info["override_world"]
    world_dir_path = os.path.join(worlds_path, level_id)
    _make_dir(world_dir_path, override_world)
    return world_dir_path


def generate_level_dat(game_settings):
    # type: (dict) -> str
    data = NbtCompound({
        NbtString(""): NbtCompound({
            NbtString("abilities"): NbtCompound({
                NbtString("attackmobs"): NbtByte(0),
                NbtString("attackplayers"): NbtByte(0),
                NbtString("build"): NbtByte(1),
                NbtString("doorsandswitches"): NbtByte(0),
                NbtString("flying"): NbtByte(0),
                NbtString("instabuild"): NbtByte(0),
                NbtString("invulnerable"): NbtByte(0),
                NbtString("lightning"): NbtByte(0),
                NbtString("mayfly"): NbtByte(0),
                NbtString("mine"): NbtByte(1),
                NbtString("mute"): NbtByte(0),
                NbtString("noclip"): NbtByte(0),
                NbtString("op"): NbtByte(0),
                NbtString("opencontainers"): NbtByte(0),
                NbtString("teleport"): NbtByte(0)
            }),
            NbtString("permissionsLevel"): NbtInt(0),
            NbtString("playerPermissionsLevel"): NbtInt(1),
            NbtString("flySpeed"): NbtFloat(0.0500000007451),
            NbtString("walkSpeed"): NbtFloat(0.10000000149),
            # NbtString("lastOpenedWithVersion"): NbtList([
            #     NbtInt(1),
            #     NbtInt(12),
            #     NbtInt(0),
            #     NbtInt(0),
            #     NbtInt(0)
            # ]),
            # NbtString("MinimumCompatibleClientVersion"): NbtList([
            #     NbtInt(1),
            #     NbtInt(12),
            #     NbtInt(0),
            #     NbtInt(0),
            #     NbtInt(0)
            # ]),
            NbtString("allowdestructiveobjects"): NbtByte(0),
            NbtString("allowmobs"): NbtByte(1),
            NbtString("bonusChestEnabled"): NbtByte(0),
            NbtString("bonusChestSpawned"): NbtByte(0),
            NbtString("CenterMapsToOrigin"): NbtByte(0),
            NbtString("cheatsEnabled"): NbtByte(game_settings["enable_cheats"]),
            NbtString("codebuilder"): NbtByte(0),
            NbtString("commandblockoutput"): NbtByte(1),
            NbtString("commandblocksenabled"): NbtByte(game_settings["command_blocks_enabled"]),
            NbtString("commandsEnabled"): NbtByte(0),
            NbtString("ConfirmedPlatformLockedContent"): NbtByte(0),
            NbtString("dodaylightcycle"): NbtByte(game_settings["do_daylight_cycle"]),
            NbtString("doentitiydrops"): NbtByte(game_settings["do_entity_drops"]),
            NbtString("dofiretick"): NbtByte(game_settings["do_fire_tick"]),
            NbtString("doimmediaterespawn"): NbtByte(game_settings["do_immediate_respawn"]),
            NbtString("doinsomnia"): NbtByte(1),
            NbtString("domobloot"): NbtByte(game_settings["do_mob_loot"]),
            NbtString("domobspawning"): NbtByte(game_settings["do_mob_spawning"]),
            NbtString("dotiledrops"): NbtByte(game_settings["do_tile_drops"]),
            NbtString("doweathercycle"): NbtByte(game_settings["do_weather_cycle"]),
            NbtString("drowningdamage"): NbtByte(1),
            NbtString("educationFeaturesEnabled"): NbtByte(0),
            NbtString("experimentalgameplay"): NbtByte(0),
            NbtString("experiments"): NbtCompound({
                NbtString("data_driven_biomes"): NbtByte(0),
                NbtString("data_driven_items"): NbtByte(0),
                NbtString("experimental_molang_features"): NbtByte(0),
                NbtString("experiments_ever_used"): NbtByte(0),
                NbtString("gametest"): NbtByte(1),
                NbtString("saved_with_toggled_experiments"): NbtByte(0),
                NbtString("upcoming_creator_features"): NbtByte(0)
            }),
            NbtString("falldamage"): NbtByte(1),
            NbtString("firedamage"): NbtByte(1),
            NbtString("freezedamage"): NbtByte(1),
            NbtString("ForceGameType"): NbtByte(1),
            NbtString("globalmute"): NbtByte(0),
            NbtString("hasBeenLoadedInCreative"): NbtByte(0),
            NbtString("hasLockedBehaviorPack"): NbtByte(0),
            NbtString("hasLockedResourcePack"): NbtByte(0),
            NbtString("immutableWorld"): NbtByte(0),
            NbtString("isFromLockedTemplate"): NbtByte(0),
            NbtString("isFromWorldTemplate"): NbtByte(0),
            NbtString("isSingleUseWorld"): NbtByte(0),
            NbtString("isWorldTemplateOptionLocked"): NbtByte(0),
            NbtString("keepinventory"): NbtByte(game_settings["keep_inventory"]),
            NbtString("LANBroadcast"): NbtByte(1),
            NbtString("LANBroadcastIntent"): NbtByte(1),
            NbtString("mobgriefing"): NbtByte(game_settings["mob_griefing"]),
            NbtString("MultiplayerGame"): NbtByte(1),
            NbtString("MultiplayerGameIntent"): NbtByte(1),
            NbtString("naturalregeneration"): NbtByte(game_settings["natural_regeneration"]),
            NbtString("pvp"): NbtByte(game_settings["pvp"]),
            NbtString("requiresCopiedPackRemovalCheck"): NbtByte(0),
            NbtString("sendcommandfeedback"): NbtByte(1),
            NbtString("showcoordinates"): NbtByte(game_settings["show_coordinates"]),
            NbtString("showdeathmessages"): NbtByte(1),
            NbtString("showtags"): NbtByte(1),
            NbtString("spawnMobs"): NbtByte(1),
            NbtString("SpawnV1Villagers"): NbtByte(0),
            NbtString("startWithMapEnabled"): NbtByte(0),
            NbtString("texturePacksRequired"): NbtByte(0),
            NbtString("tntexplodes"): NbtByte(game_settings["tnt_explodes"]),
            NbtString("useMsaGamertagsOnly"): NbtByte(0),
            NbtString("Difficulty"): NbtInt(game_settings["difficulty"]),
            NbtString("eduOffer"): NbtInt(0),
            NbtString("functioncommandlimit"): NbtInt(10000),
            NbtString("Dimension"): NbtInt(0),
            NbtString("GameType"): NbtInt(game_settings["default_game_mode"]),
            NbtString("Generator"): NbtInt(game_settings["world_type"]),
            NbtString("lightningTime"): NbtInt(0),
            NbtString("LimitedWorldOriginX"): NbtInt(0),
            NbtString("LimitedWorldOriginY"): NbtInt(0),
            NbtString("LimitedWorldOriginZ"): NbtInt(0),
            NbtString("maxcommandchainlength"): NbtInt(65535),
            NbtString("NetherScale"): NbtInt(8),
            NbtString("NetworkVersion"): NbtInt(686),
            NbtString("Platform"): NbtInt(2),
            NbtString("PlatformBroadcastIntent"): NbtInt(3),
            NbtString("rainTime"): NbtInt(0),
            NbtString("randomtickspeed"): NbtInt(1),
            NbtString("serverChunkTickRange"): NbtInt(game_settings["server_chunk_tick_range"]),
            NbtString("spawnradius"): NbtInt(game_settings["spawn_radius"]),
            NbtString("SpawnX"): NbtInt(0),
            NbtString("SpawnY"): NbtInt(64),
            NbtString("SpawnZ"): NbtInt(0),
            NbtString("StorageVersion"): NbtInt(8),
            NbtString("XBLBroadcastIntent"): NbtInt(3),
            NbtString("currentTick"): NbtLong(0),
            NbtString("LastPlayed"): NbtLong(0),
            NbtString("RandomSeed"): NbtLong(game_settings["seed"] if game_settings["seed"] else 0),
            NbtString("Time"): NbtLong(0),
            NbtString("worldStartCount"): NbtLong(0),
            NbtString("lightningLevel"): NbtFloat(0.0),
            NbtString("rainLevel"): NbtFloat(0.0),
            NbtString("baseGameVersion"): NbtString("*"),
            NbtString("BiomeOverride"): NbtString(""),
            NbtString("FlatWorldLayers"): NbtString(json.dumps({
                "biome_id": 1,
                "block_layers": [
                    {
                        "block_data": 0,
                        "block_id": 7,
                        "count": 1
                    },
                    {
                        "block_data": 0,
                        "block_id": 3,
                        "count": 2
                    },
                    {
                        "block_data": 0,
                        "block_id": 2,
                        "count": 1
                    }
                ],
                "encoding_version": 3,
                "structure_options": None
            }, indent=0, separators=(",", ":"))),
            NbtString("InventoryVersion"): NbtString(""),
            NbtString("LevelName"): NbtString(game_settings["level_name"]),
            NbtString("prid"): NbtString(""),
            NbtString("worldTemplateUUID"): NbtString(""),
            NbtString("worldTemplateVersion"): NbtString(""),
            NbtString("world_policies"): NbtCompound({})
        })
    }).deserialize()[1:-1]
    return "\x0A\x00\x00\x00" + struct.pack("<i", len(data)) + data


def deploy_level_dat(level_dat, world_dir_path):
    # type: (str, str) -> None
    with open(os.path.join(world_dir_path, "level.dat"), "wb") as ostream:
        ostream.write(level_dat)


MANIFEST_REQUIRED_KEYS = (
    ("format_version", ),
    ("modules", ),
    ("header", "uuid"),
    ("header", "version"),
    ("header", "min_engine_version"),
)
MODULE_REQUIRED_KEYS = (
    ("type", ),
    ("uuid", ),
    ("version", )
)
def _validate_manifest(manifest):
    # type: (dict) -> bool
    try:
        check_required_keys(manifest, MANIFEST_REQUIRED_KEYS)
    except Exception as exception:
        return False

    if not isinstance(manifest["modules"], list) or len(manifest["modules"]) != 1:
        return False

    module = manifest["modules"][0]
    try:
        check_required_keys(module, MODULE_REQUIRED_KEYS)
    except Exception as exception:
        return False

    if module["type"] not in ("data", "resources"):
        return False

    return True


def _recursion_find_pack(addon_dirs):
    # type: (list) -> list
    packs = []
    for addon_dir_path in addon_dirs:
        for dirpath, _, filenames in os.walk(addon_dir_path):
            if "manifest.json" in filenames:
                manifest_path = os.path.join(dirpath, "manifest.json")
                try:
                    with open(manifest_path, "r") as istream:
                        manifest = json.load(istream)

                    if _validate_manifest(manifest):
                        packs.append((dirpath, manifest["modules"][0]["type"], manifest["header"]["uuid"], manifest["header"]["version"]))
                except Exception as exception:
                    continue
    return packs


def deploy_addons(addon_dirs, world_dir_path):
    # type: (list, str) -> None
    behavior_packs = []
    resource_packs = []
    packs = _recursion_find_pack(addon_dirs)
    behavior_packs_path = os.path.join(world_dir_path, "behavior_packs")
    resource_packs_path = os.path.join(world_dir_path, "resource_packs")

    _make_dir(behavior_packs_path, True)
    _make_dir(resource_packs_path, True)
    for pack in packs:
        pack_path, pack_type, pack_uuid, pack_version = pack
        if pack_type == "data":
            behavior_packs.append({
                "type": "Addon",
                "pack_id": pack_uuid,
                "version": pack_version
            })
            shutil.copytree(pack_path, os.path.join(behavior_packs_path, pack_uuid))
        elif pack_type == "resources":
            resource_packs.append({
                "type": "Addon",
                "pack_id": pack_uuid,
                "version": pack_version
            })
            shutil.copytree(pack_path, os.path.join(resource_packs_path, pack_uuid))

    with open(os.path.join(world_dir_path, "world_behavior_packs.json"), "w") as ostream:
        ostream.write(json.dumps(behavior_packs, indent=4, separators=(',', ': ')))

    with open(os.path.join(world_dir_path, "world_resource_packs.json"), "w") as ostream:
        ostream.write(json.dumps(resource_packs, indent=4, separators=(',', ': ')))


def build_cppconfig(start_info, player_info):
    # type: (dict, dict) -> dict
    return {
        "world_info": {
            "level_id": start_info["level_id"],
        },
        "room_info": {},
        "player_info": {
            "urs": player_info["email"],
            "user_id": player_info["user_id"],
            "user_name": player_info["user_name"]
        },
        "skin_info": {
            "slim": player_info["skin"]["slim"],
            "skin": player_info["skin"]["texture"]
        }
    }


def lauch_game(start_info, cppconfig):
    # type: (dict, dict) -> None
    pe_path = start_info["pe_path"]
    if not os.path.isfile(pe_path):
        raise ValueError("{pe_path} is not a file".format(pe_path=pe_path))

    cppconfig_path = os.path.join(os.getcwd(), "cppconfig.json")
    with open(cppconfig_path, "w") as ostream:
        json.dump(cppconfig, ostream, indent=4, separators=(',', ': '))
    os.system("{pe_path} config={config}".format(pe_path=pe_path, config=cppconfig_path))


CONFIG_REQUIRED_KEYS = (
    ("addon_dirs",),
    ("start_info", "pe_path")
)
CONFIG_DEFAULT_ITEMS = {
    "start_info": {
        "override_world": True,
        "level_id": "__development_world__",
    },
    "player_info": {
        "user_id": -1,
        "user_name": "Steve",
        "email": "minecraft@netease.com",
        "skin": {
            "slim": True,
            "texture": "D:\\MCStudioDownload\\componentcache\\support\\steve\\steve.png"
        }
    },
    "world_settings": {
        "seed": 0,
        "world_type": 1,
        "level_name": "World"
    },
    "game_settings": {
        "default_game_mode": 1,
        "enable_cheats": True,
        "keep_inventory": True
    }
}
def run_with_config(config):
    # type: (dict) -> None
    # check python version(require Python2)
    check_python_version()

    # check and fill config
    check_required_keys(config, CONFIG_REQUIRED_KEYS)
    fill_default_items(config, CONFIG_DEFAULT_ITEMS)

    # build world
    start_info = config["start_info"]
    world_dir_path = build_world(start_info, get_minecraft_worlds_path())

    # deploy level.dat
    game_settings = config["game_settings"]
    level_dat = generate_level_dat(game_settings)
    deploy_level_dat(level_dat, world_dir_path)

    # deploy addons
    addon_dirs = config["addon_dirs"]
    deploy_addons(addon_dirs, world_dir_path)

    # build cppconfig
    player_info = config["player_info"]
    cppconfig = build_cppconfig(start_info, player_info)

    # launch game
    lauch_game(start_info, cppconfig)


config = read_config("config.json")
run_with_config(config)
