# Пре-сборочный шаг: web/index.html → src/webui_gz.h (gzip в PROGMEM).
#
# Зачем: страница занимала ~34 КБ флеша текстом и жила внутри C++-строки, где
# её неудобно править и легко сломать экранированием. Теперь это обычный html,
# а в прошивку уезжает сжатая копия — браузеры распаковывают gzip сами.
import gzip, os

Import("env")  # noqa

SRC = os.path.join(env.subst("$PROJECT_DIR"), "web", "index.html")   # noqa
DST = os.path.join(env.subst("$PROJECT_DIR"), "src", "webui_gz.h")   # noqa


def build():
    raw = open(SRC, "rb").read()
    # mtime=0: иначе каждая сборка даёт другие байты и файл вечно «изменён»
    packed = gzip.compress(raw, 9, mtime=0)

    out = [
        "// Сгенерировано scripts/gen_web.py из web/index.html — не править руками.",
        "// Страница отдаётся как есть, с заголовком Content-Encoding: gzip.",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "static const size_t INDEX_HTML_RAW_LEN = %d;" % len(raw),
        "static const uint8_t INDEX_HTML_GZ[] PROGMEM = {",
    ]
    for i in range(0, len(packed), 16):
        out.append("    " + ", ".join("0x%02x" % b for b in packed[i:i + 16]) + ",")
    out.append("};")
    out.append("static const size_t INDEX_HTML_GZ_LEN = sizeof(INDEX_HTML_GZ);")
    text = "\n".join(out) + "\n"

    old = open(DST).read() if os.path.exists(DST) else ""
    if old != text:
        open(DST, "w").write(text)
    print("страница: %d байт -> %d байт gzip (%.0f%%)"
          % (len(raw), len(packed), 100.0 * len(packed) / len(raw)))



# Метка сборки: время и коммит. Пишется на каждой сборке, поэтому ota.cpp,
# который её включает, пересобирается всегда, — и на странице видно именно
# время этой прошивки. __DATE__ в самом ota.cpp показывал время его последней
# пересборки: правки в других файлах дату не меняли, и по ней нельзя было
# понять, встало ли обновление.
def build_info():
    import subprocess, time
    try:
        rev = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"],
                                      cwd=env.subst("$PROJECT_DIR"), text=True).strip()   # noqa
        dirty = subprocess.call(["git", "diff", "--quiet", "HEAD"], cwd=env.subst("$PROJECT_DIR"))   # noqa
        rev += "+правки" if dirty else ""
    except Exception:
        rev = "без git"
    stamp = time.strftime("%d.%m.%Y %H:%M")
    dst = os.path.join(env.subst("$PROJECT_DIR"), "src", "build_info.h")   # noqa
    open(dst, "w").write('// Сгенерировано scripts/gen_web.py на каждой сборке — не править.\n'
                         '#pragma once\n#define BUILD_INFO "%s, %s"\n' % (stamp, rev))


build()
build_info()
