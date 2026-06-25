# -*- coding: utf-8 -*-
import math

import mod.client.extraClientApi as clientApi

from .util import as_int, json_safe


FACE_DOWN = 0
FACE_UP = 1
FACE_NORTH = 2
FACE_SOUTH = 3
FACE_WEST = 4
FACE_EAST = 5
EYE_HEIGHT = 1.62


def calc_aim(eye_pos, target):
    dx = target[0] - eye_pos[0]
    dy = target[1] - eye_pos[1]
    dz = target[2] - eye_pos[2]
    horiz = math.sqrt(dx * dx + dz * dz)
    yaw = math.atan2(-dx, dz) * 180.0 / math.pi
    pitch = -math.atan2(dy, horiz) * 180.0 / math.pi
    return pitch, yaw


class GameControl(object):
    def factory(self):
        return clientApi.GetEngineCompFactory()

    def level_id(self):
        return clientApi.GetLevelId()

    def player_id(self):
        return clientApi.GetLocalPlayerId()

    def player_pos(self):
        pos = self.factory().CreatePos(self.player_id()).GetFootPos()
        if not pos:
            return None
        return [pos[0], pos[1], pos[2]]

    def player_rot(self):
        rot = self.factory().CreateRot(self.player_id()).GetRot()
        if not rot:
            return None
        return {"pitch": rot[0], "yaw": rot[1]}

    def set_rot(self, pitch, yaw):
        self.factory().CreateRot(self.player_id()).SetRot((float(pitch), float(yaw)))
        return {"pitch": float(pitch), "yaw": float(yaw)}

    def look_at(self, target):
        pos = self.player_pos()
        if not pos:
            raise RuntimeError("Cannot get player position")
        eye = (pos[0], pos[1] + EYE_HEIGHT, pos[2])
        pitch, yaw = calc_aim(eye, target)
        return self.set_rot(pitch, yaw)

    def lock_input(self, strafe=0.0, forward=1.0):
        self.factory().CreateActorMotion(self.player_id()).LockInputVector((float(strafe), float(forward)))
        return {"input": [float(strafe), float(forward)]}

    def unlock_input(self):
        self.factory().CreateActorMotion(self.player_id()).UnlockInputVector()
        return {"unlocked": True}

    def jump(self):
        clientApi.SimulateJump()
        return {"jumped": True}

    def use_item(self):
        try:
            import localplayermodule
            if hasattr(localplayermodule, "local_player_use_item"):
                result = localplayermodule.local_player_use_item()
                using = localplayermodule.local_player_is_using_item() if hasattr(localplayermodule, "local_player_is_using_item") else None
                return {"result": json_safe(result), "is_using_item": json_safe(using)}
        except Exception as exc:
            raise RuntimeError("use item failed: " + str(exc))
        raise RuntimeError("local_player_use_item is not available in this runtime")

    def release_item(self):
        try:
            import localplayermodule
            if hasattr(localplayermodule, "local_player_release_using_item"):
                result = localplayermodule.local_player_release_using_item()
                using = localplayermodule.local_player_is_using_item() if hasattr(localplayermodule, "local_player_is_using_item") else None
                return {"result": json_safe(result), "is_using_item": json_safe(using)}
        except Exception as exc:
            raise RuntimeError("release item failed: " + str(exc))
        raise RuntimeError("local_player_release_using_item is not available in this runtime")

    def selected_slot(self):
        try:
            return self.factory().CreateItem(self.player_id()).GetSlotId()
        except Exception:
            return None

    def select_slot(self, slot):
        slot = as_int(slot, 0, 0, 8)
        try:
            import localplayermodule
            if hasattr(localplayermodule, "local_player_select_slot"):
                result = localplayermodule.local_player_select_slot(slot)
                return {"slot": slot, "result": json_safe(result)}
        except Exception as exc:
            raise RuntimeError("select slot failed: " + str(exc))
        raise RuntimeError("local_player_select_slot is not available in this runtime")

    def carried_item(self):
        try:
            item_comp = self.factory().CreateItem(self.player_id())
            carried = item_comp.GetCarriedItem(True) if hasattr(item_comp, "GetCarriedItem") else None
            if carried:
                return json_safe(carried)
        except Exception:
            pass
        try:
            enum = clientApi.GetMinecraftEnum()
            carried_items = self.factory().CreateItem(self.player_id()).GetPlayerAllItems(enum.ItemPosType.CARRIED)
            if carried_items and carried_items[0]:
                return json_safe(carried_items[0])
        except Exception:
            pass
        return None

    def use_item_on_block(self, pos, face=FACE_UP):
        try:
            import localplayermodule
            if hasattr(localplayermodule, "local_player_build_block"):
                result = localplayermodule.local_player_build_block(int(pos[0]), int(pos[1]), int(pos[2]), int(face))
                return {"pos": [int(pos[0]), int(pos[1]), int(pos[2])], "face": int(face), "result": json_safe(result)}
        except Exception as exc:
            raise RuntimeError("use item on block failed: " + str(exc))
        raise RuntimeError("local_player_build_block is not available in this runtime")

    def attack_entity(self, entity_id):
        try:
            import localplayermodule
            if hasattr(localplayermodule, "local_player_attack_entity"):
                result = localplayermodule.local_player_attack_entity(entity_id)
                return {"entity_id": entity_id, "result": json_safe(result)}
        except Exception as exc:
            raise RuntimeError("attack entity failed: " + str(exc))
        raise RuntimeError("local_player_attack_entity is not available in this runtime")
