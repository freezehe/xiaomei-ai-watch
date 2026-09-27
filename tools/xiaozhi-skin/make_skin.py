"""
XiaoZhi 皮肤生成工具（ESP32-S3-Touch-AMOLED-1.75C，466x466 圆屏）

1. 程序化生成背景图（星云 / 极光 / 晨曦），输出 PNG 预览
2. 转成 LVGL9 cbin 格式（lv_image_dsc_t + RGB565，带有序抖动防止渐变色带）
3. 解包小智编译出的 generated_assets.bin，加入背景图并在 index.json 写入 skin，再重新打包

用法:
  python make_skin.py preview                       # 只生成 PNG 预览到 out/
  python make_skin.py pack --light nebula --dark aurora
      [--assets <xiaozhi-esp32>/build/generated_assets.bin]
      [--out out/assets_skin.bin]
"""
import argparse
import importlib.util
import json
import os
import struct
import tempfile

import numpy as np
from PIL import Image

SIZE = 466
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "out")
# xiaozhi-esp32 checkout: env XIAOZHI_ROOT, default = cloned next to this repo
XIAOZHI_ROOT = os.environ.get("XIAOZHI_ROOT", os.path.join(HERE, "..", "..", "..", "xiaozhi-esp32"))

# 每个风格的文字颜色：深色背景用浅色字，浅色背景用深色字
STYLES = {
    "nebula": {"text_color": "#F2F0FF", "background_color": "#0B0B1E"},
    "aurora": {"text_color": "#EAFBF7", "background_color": "#04121A"},
    "dawn":   {"text_color": "#2A2440", "background_color": "#F6E6EE"},
}


# ---------------------------------------------------------------- 绘图工具
def _grid():
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32)
    c = (SIZE - 1) / 2.0
    return x, y, np.hypot(x - c, y - c) / c  # r: 0 在圆心，1 在圆屏边缘


def _blob(img, x, y, cx, cy, radius, color, strength):
    """叠加一个高斯光斑（加色混合）"""
    d2 = ((x - cx) ** 2 + (y - cy) ** 2) / (radius ** 2)
    img += np.exp(-d2)[..., None] * np.array(color, np.float32) * strength


def _stars(img, rng, count, max_bright, flare_count=0):
    """随机星点；少数亮星带十字星芒"""
    x, y, _ = _grid()
    for i in range(count):
        sx, sy = rng.uniform(0, SIZE, 2)
        b = rng.uniform(0.15, 1.0) ** 3 * max_bright
        tint = np.array([1.0, rng.uniform(0.85, 1.0), rng.uniform(0.85, 1.05)], np.float32)
        r = rng.uniform(0.6, 1.3)
        x0, x1 = int(max(sx - 6, 0)), int(min(sx + 7, SIZE))
        y0, y1 = int(max(sy - 6, 0)), int(min(sy + 7, SIZE))
        d2 = (x[y0:y1, x0:x1] - sx) ** 2 + (y[y0:y1, x0:x1] - sy) ** 2
        img[y0:y1, x0:x1] += np.exp(-d2 / (r * r))[..., None] * tint * b
    for i in range(flare_count):
        sx, sy = rng.uniform(40, SIZE - 40, 2)
        dx, dy = np.abs(x - sx), np.abs(y - sy)
        core = np.exp(-(dx ** 2 + dy ** 2) / 4.0)
        spikes = np.exp(-dy ** 2 / 0.8) * np.exp(-dx / 14.0) + np.exp(-dx ** 2 / 0.8) * np.exp(-dy / 14.0)
        img += (core * 1.2 + spikes * 0.45)[..., None] * np.array([1.0, 0.97, 1.0], np.float32)


def _fbm(rng, octaves=((90, 1.0), (40, 0.55), (16, 0.3), (6, 0.15))):
    """分形噪声：多个尺度的高斯模糊随机场叠加，均值 0、标准差约 1"""
    total = np.zeros((SIZE, SIZE), np.float32)
    for sigma, weight in octaves:
        # 频域高斯低通（噪声是随机场，周期边界无影响）
        n = rng.normal(0, 1, (SIZE, SIZE))
        f = np.fft.fftfreq(SIZE)
        lowpass = np.exp(-2 * (np.pi * sigma) ** 2 * (f[:, None] ** 2 + f[None, :] ** 2))
        n = np.real(np.fft.ifft2(np.fft.fft2(n) * lowpass)).astype(np.float32)
        total += (n / (n.std() + 1e-6)) * weight
    return total / total.std()


def _vignette(img, r, inner=0.78, outer=1.02, floor=0.0):
    """向圆屏边缘压暗，AMOLED 边缘纯黑更省电也更有质感"""
    t = np.clip((r - inner) / (outer - inner), 0, 1)
    k = 1 - (t * t * (3 - 2 * t)) * (1 - floor)
    img *= k[..., None]


def _darken_bottom(img, y, start=0.68, amount=0.35):
    """底部聊天字幕区域略微压暗，保证文字可读"""
    t = np.clip((y / SIZE - start) / (1 - start), 0, 1)
    img *= (1 - amount * t)[..., None]


def render(style, seed=7):
    rng = np.random.default_rng(seed)
    x, y, r = _grid()
    img = np.zeros((SIZE, SIZE, 3), np.float32)

    if style == "nebula":
        # 深空底色：上蓝下紫的纵向渐变
        t = (y / SIZE)[..., None]
        img += (1 - t) * np.array([0.03, 0.04, 0.12]) + t * np.array([0.07, 0.02, 0.10])
        # 星云：几团不同颜色的柔光，围绕中心但避开正中（给表情留位置）
        _blob(img, x, y, 130, 150, 150, (0.55, 0.18, 0.75), 0.55)   # 紫
        _blob(img, x, y, 350, 120, 120, (0.10, 0.45, 0.85), 0.50)   # 蓝
        _blob(img, x, y, 330, 340, 140, (0.85, 0.20, 0.55), 0.40)   # 品红
        _blob(img, x, y, 150, 360, 110, (0.15, 0.70, 0.80), 0.30)   # 青
        _blob(img, x, y, 233, 233, 90, (0.60, 0.55, 0.95), 0.35)    # 中心柔光，托住表情
        # 星云纹理：分形噪声调制亮度，并用第二层噪声让颜色在紫/蓝之间漂移
        density = _fbm(rng)
        img *= np.clip(1 + 0.35 * density, 0.25, 2.0)[..., None]
        hue = _fbm(rng, ((70, 1.0), (25, 0.4)))
        img += (np.clip(hue, 0, None) * 0.05)[..., None] * np.array([0.3, 0.1, 0.6], np.float32)
        # 暗色尘埃带：斜向的一条稀疏暗纹，增加层次
        dust = np.exp(-((x - y * 0.6 - 120) / 45.0) ** 2) * np.clip(_fbm(rng, ((12, 1.0), (5, 0.5))), 0, None)
        img *= (1 - 0.35 * np.clip(dust, 0, 1))[..., None]
        _stars(img, rng, 420, 0.9, flare_count=4)
        _darken_bottom(img, y)
        _vignette(img, r)

    elif style == "aurora":
        t = (y / SIZE)[..., None]
        img += (1 - t) * np.array([0.01, 0.05, 0.09]) + t * np.array([0.00, 0.02, 0.04])
        # 极光：三条正弦形光带，颜色从绿到青到紫；帘幕细纹用一维噪声，避免规则条纹
        ray = _fbm(rng, ((3, 1.0), (1.2, 0.5)))[0]
        for i, (col, amp, phase, base, width, k) in enumerate([
            ((0.10, 0.95, 0.55), 38, 0.0, 150, 26, 0.9),
            ((0.10, 0.75, 0.95), 30, 1.7, 190, 22, 0.6),
            ((0.65, 0.30, 0.95), 26, 3.1, 120, 20, 0.45),
        ]):
            center = base + amp * np.sin(x / SIZE * 2 * np.pi * 1.1 + phase) + 12 * np.sin(x / 37.0 + i)
            above = np.clip(center - y, 0, None)          # 光带上方拖出长长的光幕
            below = np.clip(y - center, 0, None)
            curtain = np.exp(-(below / width) ** 2) * np.exp(-above / (width * 3.2))
            # 竖直细纹，模拟极光的"帘幕"质感
            streak = np.clip(0.8 + 0.22 * np.roll(ray, i * 57)[None, :], 0.3, 1.4)
            img += (curtain * streak)[..., None] * np.array(col, np.float32) * k
        # 地平线处的雪山剪影
        ridge = 380 + 18 * np.sin(x / 41.0) + 10 * np.sin(x / 13.0 + 1.3) + 6 * np.sin(x / 7.0)
        mountain = (y > ridge)[..., None]
        img = np.where(mountain, img * 0.12 + np.array([0.02, 0.04, 0.06]), img)
        _stars(img, rng, 300, 0.7, flare_count=2)
        _darken_bottom(img, y, start=0.72, amount=0.2)
        _vignette(img, r)

    elif style == "dawn":
        # 浅色晨曦：桃粉 -> 薰衣草 -> 天蓝，适合浅色主题（深色文字）
        t = (y / SIZE)[..., None]
        top = np.array([0.99, 0.86, 0.80])
        mid = np.array([0.90, 0.84, 0.97])
        bot = np.array([0.80, 0.90, 0.99])
        img += np.where(t < 0.5, top + (mid - top) * (t / 0.5), mid + (bot - mid) * ((t - 0.5) / 0.5))
        # 柔焦光斑（bokeh）
        for _ in range(16):
            cx, cy = rng.uniform(0, SIZE, 2)
            rad = rng.uniform(18, 60)
            d = np.hypot(x - cx, y - cy)
            disc = np.clip((rad - d) / 3.0, 0, 1) * rng.uniform(0.04, 0.10)
            img += disc[..., None] * np.array([1.0, 1.0, 1.0])
        _blob(img, x, y, 233, 233, 110, (1.0, 0.95, 1.0), 0.07)
        # 浅色主题边缘不压黑，只轻微加深
        _vignette(img, r, inner=0.85, outer=1.05, floor=0.82)
    else:
        raise ValueError(f"unknown style {style}")

    # 柔和的色调映射，避免高光截断
    img = 1 - np.exp(-img * 1.6) if style != "dawn" else img
    return np.clip(img, 0, 1)


# ---------------------------------------------------------------- 格式转换
BAYER4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32) / 16.0


def to_rgb565(img):
    """float RGB -> RGB565（Bayer 有序抖动，渐变无色带）"""
    h, w, _ = img.shape
    th = np.tile(BAYER4, (h // 4 + 1, w // 4 + 1))[:h, :w] - 0.5
    r = np.clip(np.floor(img[..., 0] * 31 + th + 0.5), 0, 31).astype(np.uint16)
    g = np.clip(np.floor(img[..., 1] * 63 + th + 0.5), 0, 63).astype(np.uint16)
    b = np.clip(np.floor(img[..., 2] * 31 + th + 0.5), 0, 31).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def to_cbin(img):
    """序列化为 cbin: lv_image_dsc_t(28B, data 字段存相对偏移) + RGB565 像素"""
    px = to_rgb565(img).astype("<u2").tobytes()
    LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_RGB565 = 0x19, 0x12
    header = struct.pack("<BBHHHHH", LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_RGB565, 0,
                         SIZE, SIZE, SIZE * 2, 0)
    dsc = header + struct.pack("<IIII", len(px), 28, 0, 0)
    assert len(dsc) == 28
    return dsc + px


def preview_png(img, path):
    """PNG 预览：按 RGB565 量化后的效果，并加圆形遮罩模拟圆屏"""
    v = to_rgb565(img)
    rgb = np.stack([(v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31], -1).astype(np.uint8)
    _, _, r = _grid()
    rgb[r > 1.0] = 0
    Image.fromarray(rgb).save(path)


# ---------------------------------------------------------------- assets 解包/打包
def unpack_assets(path, name_len=32):
    data = open(path, "rb").read()
    count, _checksum, length = struct.unpack_from("<III", data, 0)
    entry = name_len + 12
    table, body = data[12:12 + count * entry], data[12 + count * entry:]
    files = {}
    for i in range(count):
        e = table[i * entry:(i + 1) * entry]
        name = e[:name_len].split(b"\0", 1)[0].decode()
        size, offset = struct.unpack_from("<II", e, name_len)
        assert body[offset:offset + 2] == b"\x5a\x5a", f"bad magic for {name}"
        files[name] = body[offset + 2:offset + 2 + size]
    return files


def load_packer():
    spec = importlib.util.spec_from_file_location(
        "bda", os.path.join(XIAOZHI_ROOT, "scripts", "build_default_assets.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.pack_assets_simple


def pack(light, dark, assets_in, out_path):
    # 固件 main/assets.cc 的 mmap_assets_table::asset_name[32]（与 CONFIG_MMAP_FILE_NAME_LENGTH 无关）
    name_len = 32
    files = unpack_assets(assets_in, name_len)
    index = json.loads(files["index.json"])
    skin = {}
    for mode, style in (("light", light), ("dark", dark)):
        name = f"bg_{style}.bin"
        assert len(name) <= name_len, f"{name} 超过 {name_len} 字节文件名上限"
        files[name] = to_cbin(render(style))
        skin[mode] = dict(STYLES[style], background_image=name)
    index["skin"] = skin
    files["index.json"] = json.dumps(index, ensure_ascii=False, indent=4).encode()

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "assets")
        os.makedirs(src)
        for name, blob in files.items():
            open(os.path.join(src, name), "wb").write(blob)
        load_packer()(src, os.path.join(tmp, "include"), out_path, src, max_name_len=name_len)
    # 回读校验：重新解包，确认文件完整
    check = unpack_assets(out_path, name_len)
    assert all(check[n] == files[n] for n in files), "打包回读校验失败"

    size = os.path.getsize(out_path)
    assert size <= 8 * 1024 * 1024, "assets 超过 8MB 分区"
    print(f"skin: {json.dumps(skin, ensure_ascii=False)}")
    print(f"written {out_path} ({size / 1024:.0f} KB, {len(files)} files)")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("preview")
    p = sub.add_parser("pack")
    p.add_argument("--light", default="nebula", choices=STYLES)
    p.add_argument("--dark", default="aurora", choices=STYLES)
    p.add_argument("--assets", default=os.path.join(XIAOZHI_ROOT, "build", "generated_assets.bin"))
    p.add_argument("--out", default=os.path.join(OUT_DIR, "assets_skin.bin"))
    a = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    if a.cmd == "preview":
        for s in STYLES:
            path = os.path.join(OUT_DIR, f"preview_{s}.png")
            preview_png(render(s), path)
            print("preview:", path)
    else:
        pack(a.light, a.dark, a.assets, a.out)


if __name__ == "__main__":
    main()
