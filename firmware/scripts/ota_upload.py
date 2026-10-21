#!/usr/bin/env python3
# Прошивка модуля по сети тем же путём, что кнопка на странице:
# /api/ota/begin → куски (размер называет модуль) → /api/ota/end.
# Кусок, потерянный слабым Wi-Fi, повторяется — до 20 раз.
#
#   python3 scripts/ota_upload.py 192.168.1.117 '<пароль модуля>' .pio/build/esp32s3-hs35/firmware.bin
#
# После end модуль перезагружается; что новая сборка встала — видно по полю
# "fw" в /api/state. Не загрузилась — загрузчик сам вернёт прежнюю.
import sys, json, time, urllib.request, urllib.parse
if len(sys.argv) != 4: sys.exit("ota_upload.py <адрес> <пароль> <firmware.bin>")
host, pw, path = sys.argv[1], urllib.parse.quote(sys.argv[2], safe=""), sys.argv[3]
data = open(path, "rb").read()
def call(method, url, body=None, timeout=30, hdr=None):
    req = urllib.request.Request("http://%s%s" % (host, url), data=body, method=method,
                                 headers=dict({"Content-Type": "application/octet-stream"} if body else {}, **(hdr or {})))
    with urllib.request.urlopen(req, timeout=timeout) as r: return json.loads(r.read() or b"{}")
r = call("GET", "/api/ota/begin?size=%d&pass=%s" % (len(data), pw)); print("begin:", r)
part, off, t0, retries = r.get("part", 16384), 0, time.time(), 0
while off < len(data):
    try:
        r = call("POST", "/api/ota/part", data[off:off + part], hdr={"X-Off": str(off), "X-Pass": pw})
    except Exception as e:
        retries += 1; print("повтор куска", off, e, flush=True); time.sleep(1.5)
        if retries > 20: sys.exit("слишком много повторов")
        continue
    if not r.get("ok"):
        if r.get("retry") and retries < 20: retries += 1; continue
        sys.exit("отказ: %s" % r)
    off = r["next"]
print("передано %d байт за %.0f с, повторов %d" % (off, time.time() - t0, retries))
try: print("end:", call("GET", "/api/ota/end?pass=%s" % pw, timeout=40))
except Exception as e: print("end: ответ потерялся (%s) — проверим по сборке" % e)
