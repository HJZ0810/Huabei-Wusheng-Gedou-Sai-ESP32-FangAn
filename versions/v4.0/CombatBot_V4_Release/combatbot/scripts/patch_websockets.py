"""对固定 WebSockets 2.7.2 应用最小 ESP32 兼容补丁，不修改用户全局库。

core2 的 setTimeout 使用秒，core3 使用毫秒；TLS 握手限制为8秒。
ESP32 不提供 CA 时拒绝连接，接收帧限制为2048字节。原库 LGPL 保持不变。
"""
from pathlib import Path
import hashlib
import json

ORIGINAL = {
    "WebSocketsClient.cpp": "81c6900bf87414bdd63dee8ef5954af960b2fed70a162d48d4e99eefa959ae82",
    "WebSockets.h": "99447004878caf123010eb7e4453f0f7c35ecaac1d5ebfc6b0a875f8103af665",
}
LEGACY_PATCHED = {
    "WebSocketsClient.cpp": "99eaf891ad217730bc3d29f33a18183cab984ef42f1923dea6914b117b36173f",
    "WebSockets.h": "d72c967c5563ad3d42d4eae36c31c5e4451b427546d4710caba34ff7a1353857",
}
PATCHED = {
    "WebSocketsClient.cpp": "29b96e2d3d5adf6726574233d9d033c76f470a1ed07adfddb579e25518cc4a42",
    "WebSockets.h": "aa6819f9c83611aef63135fca830bc567a129be302a73c393436eae44e84162a",
}
NOTICES = {
    "WebSocketsClient.cpp": """/*
 * Modified by CombatBot maintainers on 2026-10-06.
 * ESP32 changes: version-aware TCP timeout units, 8-second TLS handshake
 * timeout, and mandatory CA verification when no fingerprint is configured.
 * These modifications retain GNU LGPL 2.1-or-later; see the original notice
 * below and the accompanying LICENSE file.
 */

""",
    "WebSockets.h": """/*
 * Modified by CombatBot maintainers on 2026-10-06.
 * ESP32 change: limit received WebSocket frame payloads to 2048 bytes.
 * These modifications retain GNU LGPL 2.1-or-later; see the original notice
 * below and the accompanying LICENSE file.
 */

""",
}


def patch_library(base):
    base = Path(base)
    records = {}
    for name, digest in ORIGINAL.items():
        path = base / "src" / name
        data = path.read_bytes()
        actual = hashlib.sha256(data).hexdigest()
        if actual == PATCHED.get(name):
            records[name] = {"original": digest, "patched": actual}
            continue
        if actual not in {digest, LEGACY_PATCHED[name]}:
            raise RuntimeError(f"WebSockets 2.7.2 来源或补丁不匹配：{name}")
        crlf = b"\r\n" in data
        text = data.decode("utf-8").replace("\r\n", "\n")
        if actual == digest and name == "WebSocketsClient.cpp":
            old = "    _client.tcp->setTimeout(WEBSOCKETS_TCP_TIMEOUT);"
            new = """#if defined(ESP32) && ESP_ARDUINO_VERSION_MAJOR < 3
    _client.tcp->setTimeout((WEBSOCKETS_TCP_TIMEOUT + 999) / 1000);
#else
    _client.tcp->setTimeout(WEBSOCKETS_TCP_TIMEOUT);
#endif"""
            assert text.count(old) == 1
            text = text.replace(old, new)
            old = "                _client.ssl->setCACert(_CA_cert);\n#elif defined(ESP8266)"
            assert text.count(old) == 1
            text = text.replace(old, "                _client.ssl->setHandshakeTimeout(8);\n" + old)
            old = "            } else if(!SSL_FINGERPRINT_IS_SET) {\n                _client.ssl->setInsecure();"
            assert text.count(old) == 1
            text = text.replace(old, "            } else if(!SSL_FINGERPRINT_IS_SET) {\n                clientDisconnect(&_client, \"CA required\");\n                return;")
        elif actual == digest:
            old = "#define WEBSOCKETS_MAX_DATA_SIZE (15 * 1024)"
            assert old in text
            text = text.replace(old, "#define WEBSOCKETS_MAX_DATA_SIZE (2048)", 1)
        # LGPL 2.1 §2(b)：每份修改文件须显著注明修改者与日期；原版权保留。
        # 仅识别的原版/早期补丁可升级，新版经哈希匹配后直接返回，避免重复声明。
        text = NOTICES[name] + text
        output = (text.replace("\n", "\r\n") if crlf else text).encode("utf-8")
        if hashlib.sha256(output).hexdigest() != PATCHED[name]:
            raise RuntimeError(f"补丁输出与审计基线不匹配：{name}")
        path.write_bytes(output)
        records[name] = {"original": digest, "patched": hashlib.sha256(output).hexdigest()}
    return records


if __name__ == "__main__" or "__file__" not in globals():
    if "__file__" in globals():
        root = Path(__file__).resolve().parents[1]
    else:
        Import("env")
        root = Path(env.subst("$PROJECT_DIR"))
    result = patch_library(root / ".pio/libdeps/esp32s3/WebSockets")
    print("WebSockets fixed dependency patch: " + json.dumps(result, ensure_ascii=False))
