# -*- coding: utf-8 -*-
import math
import traceback

import mod.client.extraClientApi as clientApi

from .util import as_int, safe_text


EYE_HEIGHT = 1.62


class WorldProbe(object):
    def __init__(self):
        pass

    def factory(self):
        return clientApi.GetEngineCompFactory()

    def level_id(self):
        return clientApi.GetLevelId()

    def player_id(self):
        return clientApi.GetLocalPlayerId()

    def vec3(self, value):
        if not value:
            return None
        return [value[0], value[1], value[2]]

    def block_at(self, pos):
        try:
            result = self.factory().CreateBlockInfo(self.level_id()).GetBlock((int(pos[0]), int(pos[1]), int(pos[2])))
        except Exception:
            result = None
        if not result:
            return {"name": "unknown", "aux": 0}
        return {"name": safe_text(result[0]), "aux": result[1] if len(result) > 1 else 0}

    def entity_pos(self, entity_id):
        try:
            pos = self.factory().CreatePos(entity_id).GetFootPos()
        except Exception:
            pos = None
        return self.vec3(pos)

    def player(self):
        pid = self.player_id()
        factory = self.factory()
        pos = None
        rot = None
        try:
            pos = factory.CreatePos(pid).GetFootPos()
        except Exception:
            pass
        try:
            rot = factory.CreateRot(pid).GetRot()
        except Exception:
            pass
        return {
            "entity_id": safe_text(pid),
            "pos": self.vec3(pos),
            "rot": {"pitch": rot[0], "yaw": rot[1]} if rot else None,
        }

    def inventory(self, include_inventory=True):
        if not include_inventory:
            return {"held_item": None, "inventory": None}
        pid = self.player_id()
        try:
            item_comp = self.factory().CreateItem(pid)
            enum = clientApi.GetMinecraftEnum()
            carried = item_comp.GetPlayerAllItems(enum.ItemPosType.CARRIED)
            inventory = item_comp.GetPlayerAllItems(enum.ItemPosType.INVENTORY)
        except Exception:
            return {"held_item": None, "inventory": []}

        def item_to_json(slot, item):
            if not item or not item.get("itemName"):
                return None
            return {
                "slot": slot,
                "name": item.get("itemName", "air"),
                "count": item.get("count", 0),
                "aux": item.get("auxValue", 0),
            }

        held = item_to_json(0, carried[0]) if carried and carried[0] else None
        inv = []
        if inventory:
            for slot in range(len(inventory)):
                item = item_to_json(slot, inventory[slot])
                if item:
                    inv.append(item)
        return {"held_item": held, "inventory": inv}

    def entities(self, radius):
        pid = self.player_id()
        factory = self.factory()
        try:
            entity_ids = factory.CreateGame(self.level_id()).GetEntitiesAround(pid, int(radius), {})
        except Exception:
            return []
        if not entity_ids:
            return []

        player_pos = None
        try:
            player_pos = factory.CreatePos(pid).GetFootPos()
        except Exception:
            pass

        result = []
        for entity_id in entity_ids:
            if entity_id == pid:
                continue
            entity_pos = None
            entity_type = "unknown"
            try:
                entity_pos = factory.CreatePos(entity_id).GetFootPos()
            except Exception:
                pass
            try:
                type_comp = factory.CreateEngineType(entity_id)
                entity_type = type_comp.GetEngineTypeStr() or "unknown"
            except Exception:
                pass
            item = {"entity_id": safe_text(entity_id), "type": entity_type, "pos": self.vec3(entity_pos)}
            if player_pos and entity_pos:
                dx = entity_pos[0] - player_pos[0]
                dy = entity_pos[1] - player_pos[1]
                dz = entity_pos[2] - player_pos[2]
                item["distance"] = round((dx * dx + dy * dy + dz * dz) ** 0.5, 2)
            result.append(item)
        result.sort(key=lambda x: x.get("distance", 999999))
        return result

    def yaw_vectors(self, yaw):
        rad = math.radians(yaw)
        forward = (-math.sin(rad), math.cos(rad))
        right = (math.cos(rad), math.sin(rad))
        return forward, right

    def sample_block(self, label, base_pos, dx, dy, dz):
        pos = [int(math.floor(base_pos[0] + dx)), int(math.floor(base_pos[1] + dy)), int(math.floor(base_pos[2] + dz))]
        return {"label": label, "pos": pos, "block": self.block_at(pos)}

    def spatial_samples(self, player=None):
        player = player or self.player()
        pos = player.get("pos")
        rot = player.get("rot") or {"yaw": 0.0}
        if not pos:
            return []
        yaw = rot.get("yaw", 0.0)
        forward, right = self.yaw_vectors(yaw)
        samples = [
            self.sample_block("feet", pos, 0, 0, 0),
            self.sample_block("below", pos, 0, -1, 0),
            self.sample_block("head", pos, 0, 1, 0),
            self.sample_block("front_feet_1", pos, forward[0], 0, forward[1]),
            self.sample_block("front_head_1", pos, forward[0], 1, forward[1]),
            self.sample_block("front_feet_2", pos, forward[0] * 2, 0, forward[1] * 2),
            self.sample_block("left_feet_1", pos, -right[0], 0, -right[1]),
            self.sample_block("right_feet_1", pos, right[0], 0, right[1]),
        ]
        return samples

    def pseudo_vision(
        self,
        radius,
        include_blocks=False,
        include_entities=True,
        player=None,
        entities_cache=None,
        blocks_summary_cache=None,
    ):
        radius = as_int(radius, 8, 1, 32)
        vision = {
            "radius": radius,
            "spatial_samples": self.spatial_samples(player),
        }
        if include_entities:
            vision["entities"] = entities_cache if entities_cache is not None else self.entities(radius)
        summary_radius = radius if include_blocks else min(radius, 6)
        if blocks_summary_cache is not None and include_blocks:
            vision["blocks_summary"] = blocks_summary_cache
        else:
            vision["blocks_summary"] = self.blocks_summary(summary_radius)
        return vision

    def blocks_summary(self, radius):
        radius = as_int(radius, 4, 1, 16)
        factory = self.factory()
        try:
            pos = factory.CreatePos(self.player_id()).GetFootPos()
        except Exception:
            pos = None
        if not pos:
            return {"partial": True, "total": 0, "blocks": {}}

        px, py, pz = int(pos[0]), int(pos[1]), int(pos[2])
        start_pos = (px - radius, max(py - radius, -64), pz - radius)
        end_pos = (px + radius, min(py + radius, 319), pz + radius)
        try:
            palette = factory.CreateBlock(self.level_id()).GetBlockPaletteBetweenPos(start_pos, end_pos)
        except Exception:
            traceback.print_exc()
            palette = None
        if not palette:
            return {"partial": True, "total": 0, "blocks": {}}

        descriptions = {}
        try:
            descriptions.update(getattr(palette, "mBlockPaletteDescriptionsDict", {}) or {})
            descriptions.update(getattr(palette, "mExtraPaletteDescriptionsDict", {}) or {})
        except Exception:
            pass

        blocks = {}
        total = 0
        for key, positions in descriptions.items():
            name = key[0] if isinstance(key, tuple) else safe_text(key)
            count = len(positions) if isinstance(positions, list) else 0
            if count <= 0:
                continue
            blocks[name] = blocks.get(name, 0) + count
            total += count
        return {"partial": False, "total": total, "blocks": blocks}

    def observe(self, radius=8, include_blocks=False, include_entities=True, include_inventory=True, tasks=None, events=None):
        radius = as_int(radius, 8, 1, 32)
        player = self.player()
        data = {
            "player": player,
            "tasks": tasks or [],
            "recent_events": events or [],
        }
        data.update(self.inventory(include_inventory))
        entities_result = None
        if include_entities:
            entities_result = self.entities(radius)
            data["entities"] = entities_result
        blocks_result = None
        if include_blocks:
            blocks_result = self.blocks_summary(radius)
            data["blocks_summary"] = blocks_result
        data["pseudo_vision"] = self.pseudo_vision(
            radius,
            include_blocks,
            include_entities,
            player,
            entities_result,
            blocks_result,
        )
        return data
