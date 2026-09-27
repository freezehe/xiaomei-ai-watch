"""
Generate the subset fonts used by the Clock app (components/brookesia_app_clock/fonts/).

Only the glyphs the app can display are included, so the fonts stay small:
  clock_font_cn_28 / clock_font_cn_20 : Noto Sans SC Medium (SIL OFL), Chinese date / lunar / status text
  clock_font_digits_120               : Montserrat Medium (SIL OFL, shipped with LVGL), big "HH:MM"

Requires Node.js (npx downloads lv_font_conv on first use):
    python tools/make_clock_fonts.py

If you add new Chinese text to the Clock app, add it to CN_TEXT below and run the script again.
"""
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "components", "brookesia_app_clock", "fonts")

NOTO_SC = r"C:\Windows\Fonts\Noto Sans SC Medium (TrueType).otf"
MONTSERRAT = os.path.join(ROOT, "managed_components", "lvgl__lvgl", "scripts", "built_in_font", "Montserrat-Medium.ttf")

# Every Chinese string the app (and lunar.c) can produce
CN_TEXT = (
    "年月日星期周一二三四五六七八九十"                      # Gregorian date, weekday
    "农历正冬腊闰初廿三"                                     # lunar month/day names
    "甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥"           # sexagenary cycle
    "鼠牛虎兔龙蛇马羊猴鸡狗猪"                               # zodiac
    "除夕春节元宵端午七夕中元秋重阳腊八旦劳动儿童国庆圣诞"   # festivals
    "已同步北京时间未校准正在联网等待网络配置点击切换表盘"   # status / hints
)
ASCII = "0x20-0x7E"
EXTRA = "0xB7"   # "·"


def run(args):
    cmd = ["npx", "--yes", "lv_font_conv@1.5.3", "--format", "lvgl", "--lv-include", "lvgl.h"] + args
    print(" ".join(cmd[:8]), "...", args[-1])
    subprocess.run(cmd, check=True, shell=(os.name == "nt"))


def cn_font(size):
    symbols = "".join(sorted(set(CN_TEXT)))
    name = f"clock_font_cn_{size}"
    run(["--bpp", "4", "--size", str(size), "--no-compress",
         "--font", NOTO_SC, "--symbols", symbols, "-r", ASCII, "-r", EXTRA,
         "--lv-font-name", name, "-o", os.path.join(OUT, name + ".c")])


def digits_font(size):
    name = f"clock_font_digits_{size}"
    run(["--bpp", "4", "--size", str(size), "--no-compress",
         "--font", MONTSERRAT, "--symbols", "0123456789:-",
         "--lv-font-name", name, "-o", os.path.join(OUT, name + ".c")])


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    cn_font(28)
    cn_font(20)
    digits_font(120)
