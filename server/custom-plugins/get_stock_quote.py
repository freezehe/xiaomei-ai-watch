"""
A 股 / 指数行情查询插件（腾讯财经公开接口，无需 API Key）

放到 xiaozhi-server/plugins_func/functions/ 下会被自动加载；
还需要在 data/.config.yaml 的 Intent.function_call.functions 列表里加上 get_stock_quote。

支持：
  - 指数别名：上证指数/大盘/沪指、深证成指、创业板指、沪深300、上证50、科创50、中证500、北证50
  - 个股：6 位代码（600519）或名称（茅台、宁德时代），名称通过腾讯 smartbox 搜索
  - 实时/最近收盘行情（不指定日期）
  - 指定日期（"昨天"由大模型根据上下文里的当前日期换算成 YYYY-MM-DD）；非交易日自动取之前最近的交易日
  - 最近 N 个交易日走势（days，最多 10）
"""
import re
from datetime import datetime, timedelta

import httpx

from config.logger import setup_logging
from plugins_func.register import register_function, ToolType, ActionResponse, Action

TAG = __name__
logger = setup_logging()

HEADERS = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}
TIMEOUT = httpx.Timeout(6.0, connect=3.0)

# 常用指数别名 -> 腾讯代码
INDEX_ALIASES = {
    "sh000001": ["上证指数", "上证综指", "上证", "沪指", "大盘", "a股", "A股", "股市"],
    "sz399001": ["深证成指", "深成指", "深证指数", "深指"],
    "sz399006": ["创业板指", "创业板指数", "创业板"],
    "sh000300": ["沪深300", "沪深三百"],
    "sh000016": ["上证50", "上证五十"],
    "sh000688": ["科创50", "科创板", "科创五十"],
    "sh000905": ["中证500", "中证五百"],
    "bj899050": ["北证50", "北证五十"],
}
ALIAS_TO_CODE = {a.lower(): code for code, names in INDEX_ALIASES.items() for a in names}

GET_STOCK_QUOTE_FUNCTION_DESC = {
    "type": "function",
    "function": {
        "name": "get_stock_quote",
        "description": (
            "查询中国 A 股指数或个股的行情：最新价/收盘价、涨跌幅、开盘、最高、最低、成交额。"
            "用户问上证指数、大盘、深证成指、创业板、沪深300、某只股票（名称或6位代码）的价格、涨跌、走势时调用。"
            "如果用户说了相对日期（昨天、上周五、前天），请根据当前日期换算成 YYYY-MM-DD 填入 date。"
            "你自己的知识里没有实时行情，涉及具体点位或股价时必须调用本工具，不要凭记忆回答。"
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description": "指数或股票名称/代码，例如：上证指数、创业板指、贵州茅台、600519。默认上证指数",
                },
                "date": {
                    "type": "string",
                    "description": "要查询的交易日，格式 YYYY-MM-DD。不填表示最新行情",
                },
                "days": {
                    "type": "integer",
                    "description": "查询最近几个交易日的走势（1-10），默认 1",
                },
            },
            "required": [],
        },
    },
}


def _code_from_digits(digits: str) -> str:
    """6 位代码 -> 带交易所前缀的腾讯代码"""
    if digits.startswith(("6", "9", "5")):
        return "sh" + digits
    if digits.startswith(("4", "8")):
        return "bj" + digits
    return "sz" + digits


async def _resolve(client: httpx.AsyncClient, name: str):
    """返回 (腾讯代码, 显示名)；找不到返回 (None, None)"""
    key = (name or "上证指数").strip().replace(" ", "")
    if key.lower() in ALIAS_TO_CODE:
        return ALIAS_TO_CODE[key.lower()], None
    m = re.fullmatch(r"(sh|sz|bj)?(\d{6})", key.lower())
    if m:
        return (m.group(1) + m.group(2)) if m.group(1) else _code_from_digits(m.group(2)), None

    # 名称搜索：v_hint="sh~600519~贵州茅台~gzmt~GP-A^..."
    r = await client.get("https://smartbox.gtimg.cn/s3/", params={"v": 2, "q": key, "t": "all"}, headers=HEADERS)
    text = r.content.decode("gbk", errors="ignore")
    body = text.split('"', 2)[1] if '"' in text else ""
    for item in body.split("^"):
        parts = item.split("~")
        if len(parts) >= 5 and parts[0] in ("sh", "sz", "bj") and parts[4].startswith(("GP", "ZS")):
            display = parts[2].encode().decode("unicode_escape") if "\\u" in parts[2] else parts[2]
            return parts[0] + parts[1], display
    return None, None


async def _realtime(client: httpx.AsyncClient, code: str) -> dict:
    r = await client.get(f"https://qt.gtimg.cn/q={code}", headers=HEADERS)
    f = r.content.decode("gbk", errors="ignore").split('"')[1].split("~")
    if len(f) < 38 or not f[3]:
        raise ValueError(f"no quote for {code}")
    ts = datetime.strptime(f[30][:14], "%Y%m%d%H%M%S")
    return {
        "name": f[1], "price": float(f[3]), "prev_close": float(f[4]), "open": float(f[5]),
        "change": float(f[31]), "pct": float(f[32]), "high": float(f[33]), "low": float(f[34]),
        "amount_wan": float(f[37] or 0), "time": ts,
    }


async def _daily(client: httpx.AsyncClient, code: str, count: int) -> list:
    """日 K：[(date, open, close, high, low), ...] 按日期升序"""
    r = await client.get(
        "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get",
        params={"param": f"{code},day,,,{count},qfq"}, headers=HEADERS)
    data = r.json()["data"][code]
    rows = data.get("qfqday") or data.get("day") or []
    return [(row[0], *map(float, row[1:5])) for row in rows]


def _amount_text(amount_wan: float) -> str:
    return f"{amount_wan / 1e4:.0f}亿元" if amount_wan >= 1e4 else f"{amount_wan:.0f}万元"


@register_function("get_stock_quote", GET_STOCK_QUOTE_FUNCTION_DESC, ToolType.WAIT)
async def get_stock_quote(name: str = "上证指数", date: str = None, days: int = 1):
    days = max(1, min(int(days or 1), 10))
    try:
        async with httpx.AsyncClient(timeout=TIMEOUT) as client:
            code, display = await _resolve(client, name)
            if not code:
                return ActionResponse(Action.REQLLM, f"没有找到名为「{name}」的股票或指数，请用户换个说法或提供6位代码。", None)

            quote = await _realtime(client, code)
            display = display or quote["name"]

            # 最新行情
            if not date and days == 1:
                trading_now = quote["time"].date() == datetime.now().date() and quote["time"].hour < 15
                when = quote["time"].strftime("%Y-%m-%d %H:%M")
                state = "盘中实时" if trading_now else f"{quote['time'].strftime('%Y-%m-%d')} 收盘"
                return ActionResponse(Action.REQLLM, (
                    f"{display}（{code}）{state}行情（数据时间 {when}）：\n"
                    f"最新 {quote['price']:.2f}，涨跌 {quote['change']:+.2f}（{quote['pct']:+.2f}%），"
                    f"昨收 {quote['prev_close']:.2f}，今开 {quote['open']:.2f}，"
                    f"最高 {quote['high']:.2f}，最低 {quote['low']:.2f}，成交额 {_amount_text(quote['amount_wan'])}。"
                ), None)

            # 指定日期 / 最近 N 日：多取一些，用于找前一交易日算涨跌幅
            rows = await _daily(client, code, 40)
            if len(rows) < 2:
                return ActionResponse(Action.REQLLM, f"{display} 暂时取不到历史日线数据。", None)

            if date:
                try:
                    target = datetime.strptime(date, "%Y-%m-%d").strftime("%Y-%m-%d")
                except ValueError:
                    return ActionResponse(Action.REQLLM, "日期格式应为 YYYY-MM-DD。", None)
                idx = max((i for i, r in enumerate(rows) if r[0] <= target), default=None)
                if idx is None:
                    return ActionResponse(Action.REQLLM, f"{date} 太早了，只能查最近约 40 个交易日。", None)
            else:
                idx = len(rows) - 1

            lines = []
            for i in range(max(1, idx - days + 1), idx + 1):
                d, o, c, h, l = rows[i]
                prev = rows[i - 1][2]
                lines.append(f"{d}：开 {o:.2f}，收 {c:.2f}，涨跌 {c - prev:+.2f}（{(c - prev) / prev * 100:+.2f}%），"
                             f"最高 {h:.2f}，最低 {l:.2f}")
            note = ""
            if date and rows[idx][0] != target:
                note = f"（{target} 不是交易日或尚无数据，以下为此前最近的交易日 {rows[idx][0]}）\n"
            return ActionResponse(Action.REQLLM, f"{display}（{code}）日线行情：\n{note}" + "\n".join(lines), None)

    except Exception as e:
        logger.bind(tag=TAG).error(f"查询行情失败 name={name} date={date}: {e}")
        return ActionResponse(Action.REQLLM, "行情数据暂时获取失败，请稍后再试。", None)
