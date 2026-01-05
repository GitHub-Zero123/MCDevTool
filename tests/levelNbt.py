import nbtlib
import pathlib
import io

curPath = pathlib.Path(__file__).parent.resolve()

level121 = curPath / "bins/level_1.21.dat"

with open(level121, "rb") as f:
    levelData = f.read()
    levelNbt = levelData[8:]

levelMap = \
    nbtlib.File.parse(io.BytesIO(levelNbt), byteorder="little")

print(levelMap)