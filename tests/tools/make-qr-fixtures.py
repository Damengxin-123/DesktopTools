# 生成二维码识别功能的测试图片（tests/media/qr-*.png）。
# 本机无网络，不依赖任何二维码库：按 ISO/IEC 18004 从零实现一个最小编码器，
# 只支持测试所需模式；数据块结构仅使用可独立校验的条目，写入前断言总码字一致。
# 开发期运行：python tests/tools/make-qr-fixtures.py
from PIL import Image, ImageOps
import os

# ---------- GF(256)，本原多项式 0x11D ----------
EXP = [0] * 512
LOG = [0] * 256
value = 1
for i in range(255):
    EXP[i] = value
    LOG[value] = i
    value <<= 1
    if value & 0x100:
        value ^= 0x11D
for i in range(255, 512):
    EXP[i] = EXP[i - 255]


def gmul(a, b):
    if a == 0 or b == 0:
        return 0
    return EXP[LOG[a] + LOG[b]]


# ---------- 里德-所罗门纠错码生成（仅编码方向） ----------
def rs_divisor(degree):
    # 系数按低次在前，与 Nayuki qrcodegen 的实现一致。
    result = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(len(result)):
            result[j] = gmul(result[j], root)
            if j + 1 < len(result):
                result[j] ^= result[j + 1]
        root = gmul(root, 0x02)
    return result


def rs_remainder(data, divisor):
    result = [0] * len(divisor)
    for byte in data:
        factor = byte ^ result.pop(0)
        result.append(0)
        for i, coef in enumerate(divisor):
            result[i] ^= gmul(coef, factor)
    return result


# ---------- 版本结构（写入前自校验） ----------
KNOWN_TOTALS = {1: 26, 2: 44, 3: 70, 4: 100, 5: 134, 6: 172, 7: 196}
KNOWN_REMAINDERS = {1: 0, 2: 7, 3: 7, 4: 7, 5: 7, 6: 7, 7: 0}
# (版本 -> 纠错级别 -> (每块纠错码字, [(块数, 每块数据码字), ...]))；总量写入前断言。
BLOCK_TABLE = {
    1: {"L": (7, [(1, 19)]), "H": (17, [(1, 9)])},
    2: {"M": (16, [(1, 28)])},
    3: {"Q": (18, [(2, 17)])},
    4: {"M": (18, [(2, 32)])},
    5: {"L": (26, [(1, 108)]), "Q": (18, [(2, 15), (2, 16)])},
    6: {"L": (18, [(2, 68)])},
    7: {"L": (20, [(2, 78)])},
}
KNOWN_DATA_CAPACITY = {("1", "L"): 19, ("1", "H"): 9, ("2", "M"): 28, ("3", "Q"): 34,
                       ("4", "M"): 64, ("5", "L"): 108, ("5", "Q"): 62, ("6", "L"): 136,
                       ("7", "L"): 156}


def num_raw_data_modules(version):
    result = (16 * version + 128) * version + 64
    if version >= 2:
        num_align = version // 7 + 2
        result -= (25 * num_align - 10) * num_align - 55
        if version >= 7:
            result -= 36
    return result


def alignment_positions(version):
    if version == 1:
        return []
    num_align = version // 7 + 2
    if version == 32:
        step = 26
    else:
        span = num_align * 2 - 2
        step = ((version * 4 + 4) + span - 1) // span * 2
    result = [6]
    pos = version * 4 + 17 - 7
    while len(result) < num_align:
        result.insert(1, pos)
        pos -= step
    return result


# ---------- 位流与分段 ----------
class BitBuffer:
    def __init__(self):
        self.bits = []

    def append(self, val, length):
        for shift in range(length - 1, -1, -1):
            self.bits.append((val >> shift) & 1)

    def to_bytes(self, data_codewords):
        bits = self.bits[:]
        bits.extend([0] * min(4, data_codewords * 8 - len(bits)))  # 终止符
        bits.extend([0] * (-len(bits) % 8))
        data = []
        for i in range(0, len(bits), 8):
            byte = 0
            for bit in bits[i:i + 8]:
                byte = (byte << 1) | bit
            data.append(byte)
        pad = [0xEC, 0x11]
        while len(data) < data_codewords:
            data.append(pad[len(data) % 2])
        assert len(data) == data_codewords
        return data


ALPHANUMERIC = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"


def append_numeric(buffer, text, version):
    buffer.append(0b0001, 4)
    buffer.append(len(text), 10 if version <= 9 else (12 if version <= 26 else 14))
    for i in range(0, len(text), 3):
        group = text[i:i + 3]
        buffer.append(int(group), {3: 10, 2: 7, 1: 4}[len(group)])


def append_alphanumeric(buffer, text, version):
    buffer.append(0b0010, 4)
    buffer.append(len(text), 9 if version <= 9 else (11 if version <= 26 else 13))
    values = [ALPHANUMERIC.index(ch) for ch in text]
    for i in range(0, len(values), 2):
        if i + 1 < len(values):
            buffer.append(values[i] * 45 + values[i + 1], 11)
        else:
            buffer.append(values[i], 6)


def append_byte(buffer, data, version):
    buffer.append(0b0100, 4)
    buffer.append(len(data), 8 if version <= 9 else 16)
    for byte in data:
        buffer.append(byte, 8)


def append_eci(buffer, designator):
    buffer.append(0b0111, 4)
    assert 0 <= designator < 128
    buffer.append(designator, 8)


# ---------- 矩阵绘制 ----------
class Matrix:
    def __init__(self, version):
        self.version = version
        self.size = version * 4 + 17
        self.modules = [[False] * self.size for _ in range(self.size)]
        self.is_function = [[False] * self.size for _ in range(self.size)]

    def set(self, x, y, dark):
        assert 0 <= x < self.size and 0 <= y < self.size
        self.modules[y][x] = dark
        self.is_function[y][x] = True

    def draw_finder(self, x0, y0):
        # 以 (x0+3, y0+3) 为中心的 9×9 区域：切比雪夫距离 0/1/3 为深色（核心与外框），
        # 距离 2 为浅色环，距离 4 是一圈浅色分隔符。
        for dy in range(-1, 8):
            for dx in range(-1, 8):
                x, y = x0 + dx, y0 + dy
                if not (0 <= x < self.size and 0 <= y < self.size):
                    continue
                ring = max(abs(dx - 3), abs(dy - 3))
                self.set(x, y, ring in (0, 1, 3))

    def draw_alignment(self, cx, cy):
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                ring = max(abs(dx), abs(dy))
                self.set(cx + dx, cy + dy, ring != 1)

    def draw_function_patterns(self):
        size = self.size
        self.draw_finder(0, 0)
        self.draw_finder(size - 7, 0)
        self.draw_finder(0, size - 7)
        for i in range(8, size - 8):
            self.set(i, 6, i % 2 == 0)
            self.set(6, i, i % 2 == 0)
        coords = alignment_positions(self.version)
        for cy in coords:
            for cx in coords:
                if (cx == 6 and cy == 6) or (cx == size - 7 and cy == 6) or (cx == 6 and cy == size - 7):
                    continue
                self.draw_alignment(cx, cy)
        self.set(8, size - 8, True)  # 恒黑模块。
        self.draw_format_bits(0)  # 先占位，加掩码后重绘。
        if self.version >= 7:
            self.draw_version_bits()

    def draw_format_bits(self, mask):
        ecc_bits = {"L": 1, "M": 0, "Q": 3, "H": 2}[self.ecc_level]
        data = ecc_bits << 3 | mask
        rem = data
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        bits = (data << 10 | rem) ^ 0x5412
        size = self.size
        for i in range(6):
            self.set(8, i, (bits >> i) & 1)
        self.set(8, 7, (bits >> 6) & 1)
        self.set(8, 8, (bits >> 7) & 1)
        self.set(7, 8, (bits >> 8) & 1)
        for i in range(9, 15):
            self.set(14 - i, 8, (bits >> i) & 1)
        for i in range(8):
            self.set(size - 1 - i, 8, (bits >> i) & 1)
        for i in range(8, 15):
            self.set(8, size - 15 + i, (bits >> i) & 1)
        self.set(8, size - 8, True)

    def draw_version_bits(self):
        rem = self.version
        for _ in range(12):
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25)
        bits = self.version << 12 | rem
        size = self.size
        for i in range(18):
            bit = (bits >> i) & 1
            a, b = size - 11 + i % 3, i // 3
            self.set(a, b, bit)
            self.set(b, a, bit)

    def draw_codewords(self, codewords):
        index = 0
        size = self.size
        right = size - 1
        while right >= 1:
            if right == 6:
                right = 5
            for vert in range(size):
                for j in range(2):
                    x = right - j
                    upward = (right + 1) & 2 == 0
                    y = size - 1 - vert if upward else vert
                    if not self.is_function[y][x] and index < len(codewords) * 8:
                        self.modules[y][x] = (codewords[index >> 3] >> (7 - (index & 7))) & 1
                        index += 1
            right -= 2
        assert index == len(codewords) * 8, "未放置完所有码字"

    def apply_mask(self, mask):
        formulas = [
            lambda x, y: (x + y) % 2 == 0,
            lambda x, y: y % 2 == 0,
            lambda x, y: x % 3 == 0,
            lambda x, y: (x + y) % 3 == 0,
            lambda x, y: (x // 3 + y // 2) % 2 == 0,
            lambda x, y: (x * y) % 2 + (x * y) % 3 == 0,
            lambda x, y: ((x * y) % 2 + (x * y) % 3) % 2 == 0,
            lambda x, y: ((x + y) % 2 + (x * y) % 3) % 2 == 0,
        ]
        for y in range(self.size):
            for x in range(self.size):
                if not self.is_function[y][x] and formulas[mask](x, y):
                    self.modules[y][x] = not self.modules[y][x]


def encode(segments, version, ecc_level, mask=0):
    """segments: (kind, payload) 列表，kind ∈ numeric/alnum/byte/eci26-byte。"""
    raw = num_raw_data_modules(version)
    assert raw % 8 == KNOWN_REMAINDERS[version], f"v{version} 余位校验失败"
    total = raw // 8
    assert total == KNOWN_TOTALS[version], f"v{version} 总码字校验失败"
    ecc_per_block, groups = BLOCK_TABLE[version][ecc_level]
    blocks_total = sum(count for count, _ in groups)
    data_total = sum(count * data for count, data in groups)
    assert ecc_per_block * blocks_total + data_total == total, f"v{version}-{ecc_level} 块结构与总码字不符"
    capacity = KNOWN_DATA_CAPACITY[(str(version), ecc_level)]
    assert data_total == capacity, f"v{version}-{ecc_level} 数据容量校验失败 {data_total} != {capacity}"

    buffer = BitBuffer()
    for kind, payload in segments:
        if kind == "numeric":
            append_numeric(buffer, payload, version)
        elif kind == "alnum":
            append_alphanumeric(buffer, payload, version)
        elif kind == "byte":
            append_byte(buffer, payload, version)
        elif kind == "eci26-byte":
            append_eci(buffer, 26)
            append_byte(buffer, payload, version)
        else:
            raise ValueError(kind)
    data_bits = buffer.bits
    assert len(data_bits) <= data_total * 8, "负载超过所选版本容量"

    data_codewords = buffer.to_bytes(data_total)
    # 按组拆分数据块（支持两组块尺寸不同的情况），计算纠错码字，再按规范列交织。
    block_data = []
    sizes = []
    index = 0
    for count, data_per in groups:
        for _ in range(count):
            block_data.append(data_codewords[index:index + data_per])
            sizes.append(data_per)
            index += data_per
    block_ecc = [rs_remainder(chunk, rs_divisor(ecc_per_block)) for chunk in block_data]
    interleaved = []
    for column in range(max(sizes)):
        for j, size in enumerate(sizes):
            if column < size:
                interleaved.append(block_data[j][column])
    for column in range(ecc_per_block):
        for chunk in block_ecc:
            interleaved.append(chunk[column])
    assert len(interleaved) == total

    matrix = Matrix(version)
    matrix.ecc_level = ecc_level
    matrix.draw_function_patterns()
    matrix.draw_codewords(interleaved)
    matrix.apply_mask(mask)
    matrix.draw_format_bits(mask)
    return matrix


def render(matrix, path, module_px=10, quiet=4, transforms=()):
    side = (matrix.size + quiet * 2)
    image = Image.new("L", (side * module_px, side * module_px), 255)
    pixels = image.load()
    for y in range(matrix.size):
        for x in range(matrix.size):
            if matrix.modules[y][x]:
                for py in range(module_px):
                    base = (y + quiet) * module_px
                    for px in range(module_px):
                        pixels[(x + quiet) * module_px + px, base + py] = 0
    for name, argument in transforms:
        if name == "rotate":
            image = image.rotate(argument, expand=True, fillcolor=255, resample=Image.BICUBIC)
        elif name == "resize":
            width, height = image.size
            image = image.resize((width * argument // 100, height * argument // 100), Image.LANCZOS)
        elif name == "invert":
            image = ImageOps.invert(image)
        else:
            raise ValueError(name)
    image.save(path)
    print("生成", path, image.size)


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "media")
    os.makedirs(root, exist_ok=True)

    def path(name):
        return os.path.abspath(os.path.join(root, name))

    render(encode([("byte", "https://qt.io".encode())], 1, "L"), path("qr-v1-l-url.png"))
    render(encode([("numeric", "8613800138000")], 1, "H"), path("qr-v1-h-numeric.png"))
    render(encode([("byte", "https://example.com/hello".encode())], 2, "M"), path("qr-v2-m-url.png"))
    render(encode([("byte", "https://example.com/unequal".encode())], 5, "Q"), path("qr-v5-q-blocks.png"))
    render(encode([("alnum", "HTTPS://EXAMPLE.COM/QT")], 3, "Q"), path("qr-v3-q-alnum.png"))
    render(encode([("eci26-byte", "二维码识别测试✓".encode("utf-8"))], 4, "M"), path("qr-v4-m-eciu8.png"))
    render(encode([("numeric", "2026"), ("byte", "年桌面工具".encode("utf-8"))], 5, "L"),
           path("qr-v5-l-segments.png"))
    render(encode([("byte", ("https://example.com/path?query=" + "a" * 84).encode())], 6, "L"),
           path("qr-v6-l-long.png"))
    render(encode([("byte", b"version-info-check-7")], 7, "L"), path("qr-v7-l-version.png"))

    render(encode([("byte", "https://example.com/hello".encode())], 2, "M"),
           path("qr-rot90.png"), transforms=[("rotate", 90)])
    render(encode([("byte", "https://qt.io".encode())], 1, "L"),
           path("qr-rot180.png"), transforms=[("rotate", 180)])
    render(encode([("byte", "https://example.com/hello".encode())], 2, "M"),
           path("qr-rot30.png"), transforms=[("rotate", 30)])
    render(encode([("byte", "https://example.com/hello".encode())], 2, "M"),
           path("qr-scaled50.png"), transforms=[("resize", 50)])
    render(encode([("byte", "https://qt.io".encode())], 1, "L"),
           path("qr-inverted.png"), transforms=[("invert", 0)])

    # 非二维码图片：纯色背景加简单几何图形，识别必须失败。
    negative = Image.new("L", (240, 240), 255)
    for y in range(40, 200):
        for x in range(40, 200):
            if 40 <= x < 48 or 192 <= x < 200 or 40 <= y < 48 or 192 <= y < 200:
                negative.putpixel((x, y), 0)
            elif 110 <= x < 130 and 110 <= y < 130:
                negative.putpixel((x, y), 0)
    negative.save(path("not-qr.png"))
    print("生成", path("not-qr.png"))


if __name__ == "__main__":
    main()
