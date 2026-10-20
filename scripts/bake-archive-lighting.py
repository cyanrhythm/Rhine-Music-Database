"""离线烘焙重复唱片壳的外观及选中光源的柔光场，只在构建时执行。"""

import math
import pathlib
import struct


ROOT = pathlib.Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "output/resources"
FACE_SIZE = 96
FIELD_SIZE = 64
HEIGHT_SLICES = 8
NORMALS = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (-1, 0, 0), (0, -1, 0), (0, 0, -1)]
EMITTERS = [
    (-2.22, 0.60, 0.08),
    (-2.22, 1.65, 0.08),
    (-2.22, 2.75, 0.08),
    (-0.9, 1.8, 0.08),
    (1.1, 1.8, 0.08),
]


def smoothstep(low, high, value):
    amount = min(1, max(0, (value - low) / (high - low)))
    return amount * amount * (3 - 2 * amount)


def inverse_display_curve(value):
    """将目标 sRGB 灰阶反解为最终 ACES 通道之前的线性辐亮度。"""
    target = value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4
    low = 0.0
    high = 16.0
    for _ in range(24):
        middle = (low + high) / 2
        exposed = middle * 1.02 / 0.6
        output = (exposed * (exposed + 0.0245786) - 0.000090537) / (
            exposed * (0.983729 * exposed + 0.4329510) + 0.238081
        )
        if output < target:
            low = middle
        else:
            high = middle
    return (low + high) / 2


DISPLAY_TO_LINEAR = [inverse_display_curve(index / 1023) for index in range(1024)]


def radiance(value):
    return DISPLAY_TO_LINEAR[round(min(1, max(0, value)) * 1023)]


def face_sample(face, u, v, dark):
    """在物体局部坐标中烘焙乳白端面、暖色宽面、接触暗部和内部发光条。"""
    x = u * 5 - 2.5
    y = v * 3.7
    if face in (0, 3):
        x = 2.5 if face == 0 else -2.5
    elif face in (1, 4):
        y = 3.7 if face == 1 else 0
    border = math.exp(-min(u, 1 - u, v, 1 - v) / 0.025)
    if dark:
        color = (0.14, 0.17, 0.18) if face in (0, 1, 3) else (0.075, 0.102, 0.114)
        color = tuple(channel + border * 0.035 for channel in color)
    elif face in (2, 5):
        center = (0.70 + 0.16 * v, 0.60 + 0.19 * v, 0.46 + 0.24 * v)
        edge = (0.94, 0.91, 0.85)
        color = tuple(center[index] * (1 - border * 0.65) + edge[index] * border * 0.65 for index in range(3))
    elif face == 1:
        color = (0.965, 0.95, 0.925)
    else:
        color = (0.90 + v * 0.055, 0.875 + v * 0.065, 0.825 + v * 0.085)
    height = smoothstep(0.08, 0.5, y) * (1 - smoothstep(3.25, 3.67, y))
    guide = abs(x + 2.22)
    # 仅移除窄而亮的白色核心，保留原有宽范围的暖色扩散和内部柔光。
    emission = (0.55 * math.exp(-guide / 0.48) + 0.05) * height
    return (*[radiance(channel) for channel in color], emission)


def bake_surface():
    """六个朝向共享一张昼夜图集；alpha 只保留柔和自发光，不烘焙白色窄光条。"""
    pixels = bytearray()
    for row in range(FACE_SIZE * 2):
        dark = row // FACE_SIZE != 0
        v = (row % FACE_SIZE) / (FACE_SIZE - 1)
        for column in range(FACE_SIZE * 6):
            face = column // FACE_SIZE
            u = (column % FACE_SIZE) / (FACE_SIZE - 1)
            pixels.extend(struct.pack("<4e", *face_sample(face, u, v, dark)))
    (OUTPUT / "archive-baked-surface.bin").write_bytes(pixels)


def field_sample(position, normal):
    """离线累积面积光、层间吸收和磨砂散射；运行时不再逐光源求值。"""
    x, y, z = position
    shell_count = max(abs(z) / 0.62 - 0.35, 0)
    layer = 0.78 ** min(shell_count, 12)
    result = [0.0, 0.0, 0.0]
    for index, emitter in enumerate(EMITTERS):
        offset = [emitter[axis] - position[axis] for axis in range(3)]
        distance_squared = sum(value * value for value in offset)
        inverse_distance = 1 / math.sqrt(max(distance_squared, 0.04))
        cosine = sum(normal[axis] * offset[axis] * inverse_distance for axis in range(3))
        scattering = 0.82 * (0.26 + 0.74 * max(-cosine, 0) ** 0.65)
        power = 0.85 if index < 3 else 0.28
        intensity = power * (max(cosine, 0) * 0.28 + scattering) / (0.30 + distance_squared)
        for channel, (tint, absorption) in enumerate(zip((1.0, 0.72, 0.38), (0.32, 0.90, 2.5))):
            result[channel] += tint * intensity * math.exp(-absorption * 0.12 / max(abs(cosine), 0.22))
    return (*[value * layer for value in result], 1.0)


def bake_light_field():
    """把固定局部光场的八个高度切片及六个法线方向打包成小型二维图集。"""
    pixels = bytearray()
    for row in range(FIELD_SIZE * HEIGHT_SLICES):
        height = row // FIELD_SIZE
        y = -0.4 + height * 5.2 / (HEIGHT_SLICES - 1)
        z = -7 + (row % FIELD_SIZE) * 14 / (FIELD_SIZE - 1)
        for column in range(FIELD_SIZE * 6):
            face = column // FIELD_SIZE
            x = -9 + (column % FIELD_SIZE) * 18 / (FIELD_SIZE - 1)
            pixels.extend(struct.pack("<4e", *field_sample((x, y, z), NORMALS[face])))
    (OUTPUT / "archive-baked-light.bin").write_bytes(pixels)


def bake_normal_response():
    """预积分倒角朝向的明暗与柔和高光，避免运行时计算 BRDF 或采样环境光。"""
    size = 64
    pixels = bytearray()
    for row in range(size * 2):
        dark = row >= size
        v = 2 * (row % size) / (size - 1) - 1
        for column in range(size):
            u = 2 * column / (size - 1) - 1
            normal = [u, v, 1 - abs(u) - abs(v)]
            if normal[2] < 0:
                normal[0] = (1 - abs(v)) * (1 if u >= 0 else -1)
                normal[1] = (1 - abs(u)) * (1 if v >= 0 else -1)
            length = math.sqrt(sum(value * value for value in normal))
            normal = [value / length for value in normal]
            primary_axis = max(range(3), key=lambda axis: abs(normal[axis]))
            primary_face = primary_axis + (0 if normal[primary_axis] >= 0 else 3)
            primary = face_sample(primary_face, 0.5, 0.5, dark)
            weights = [abs(value) ** 2 for value in normal]
            total = sum(weights)
            blended = [0.0, 0.0, 0.0]
            for axis, weight in enumerate(weights):
                face = axis + (0 if normal[axis] >= 0 else 3)
                color = face_sample(face, 0.5, 0.5, dark)
                for channel in range(3):
                    blended[channel] += color[channel] * weight / total
            cosine = max(0, sum(normal[axis] * (-0.48, 0.80, 0.36)[axis] for axis in range(3)))
            highlight = 0.18 * cosine**24 * (0.25 if dark else 1)
            response = [min(4, blended[channel] / max(primary[channel], 0.0001)) for channel in range(3)]
            pixels.extend(struct.pack("<4e", *response, highlight))
    (OUTPUT / "archive-baked-normal.bin").write_bytes(pixels)


def main():
    """构建期生成纯二进制线性 RGBA16F 贴图，不使用桌面图形设备或外部软件。"""
    OUTPUT.mkdir(parents=True, exist_ok=True)
    bake_surface()
    bake_light_field()
    bake_normal_response()


if __name__ == "__main__":
    main()
