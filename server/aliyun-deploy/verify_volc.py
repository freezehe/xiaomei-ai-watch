"""
火山引擎语音 开通状态检测：逐个资源尝试 WebSocket 握手（不产生识别/合成费用）

用法（本机，用服务端的虚拟环境；或任何装了 websockets+pyyaml 的 Python）：
  python verify_volc.py [配置文件路径，默认 data/.config.yaml]

结果解读：
  ✅ 已授权                              → 可以用
  ❌ 403 requested resource not granted   → 凭证对，但该服务没开通/没绑定到这个应用
  ❌ 401 / invalid                        → appid 或 access_token 填错
"""
import asyncio
import os
import sys
import uuid

import websockets
import yaml

CHECKS = [
    # (说明, 接口, resource_id, 对应配置)
    ("流式语音识别 1.0（小时版）", "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async",
     "volc.bigasr.sauc.duration", "ASR: DoubaoStreamASR（默认）"),
    ("流式语音识别 2.0（小时版）", "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async",
     "volc.seedasr.sauc.duration", "ASR: DoubaoStreamASR + resource_id 改成 volc.seedasr.sauc.duration"),
    ("语音合成大模型（湾湾小何）", "wss://openspeech.bytedance.com/api/v3/tts/bidirection",
     "volc.service_type.10029", "TTS: HuoshanDoubleStreamTTS（可选）"),
]


async def probe(url, appid, token, rid):
    headers = {"X-Api-App-Key": str(appid), "X-Api-Access-Key": str(token),
               "X-Api-Resource-Id": rid, "X-Api-Connect-Id": str(uuid.uuid4())}
    try:
        ws = await websockets.connect(url, additional_headers=headers, open_timeout=10)
        await ws.close()
        return True, "✅ 已授权"
    except websockets.InvalidStatus as e:
        body = (e.response.body or b"").decode(errors="replace")[:120]
        return False, f"❌ HTTP {e.response.status_code} {body}"
    except Exception as e:
        return False, f"❌ {type(e).__name__}: {str(e)[:120]}"


async def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join("data", ".config.yaml")
    cfg = yaml.safe_load(open(path, encoding="utf-8"))
    asr = cfg["ASR"]["DoubaoStreamASR"]
    appid, token = asr["appid"], asr["access_token"]
    if "<" in str(appid) or "<" in str(token):
        sys.exit("配置里的火山语音 appid/access_token 还是占位符，请先填写")
    print(f"appid={appid}  token=***{str(token)[-4:]}\n")
    for name, url, rid, use in CHECKS:
        _, msg = await probe(url, appid, token, rid)
        print(f"{name:18s} {rid:28s} {msg}\n{'':18s} → {use}")


if __name__ == "__main__":
    asyncio.run(main())
