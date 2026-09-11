"""
Пересобирает locale_patch под текущую версию игры - одной командой.

ЗАЧЕМ ОТДЕЛЬНЫЙ СКРИПТ. Имена js-чанков игры содержат хеш содержимого
(assets/i18n-Dh-M7E2V.js), поэтому после каждого обновления игры имена
меняются и старый патч перестаёт подходить. Сам перевод при этом никуда не
девается: locale_ru.json - это ключи, а не файлы, его переносить не надо.
Нужно лишь заново достать ассеты из нового exe и пропатчить их. Этот скрипт
делает оба шага и сразу кладёт результат рядом с DLL.

ЧТО БУДЕТ, ЕСЛИ ЗАБЫТЬ ЕГО ЗАПУСТИТЬ. Ничего страшного: DLL просто не найдёт
файлов, которые игра запрашивает, ничего не подменит, и игра пойдёт на
английском (плюс продолжит работать словарь по DOM). Сломать игру
устаревшим патчем нельзя - подменяются только файлы, чьё имя совпало
байт в байт. В translator_log.txt при этом появится предупреждение
"ни один файл из locale_patch не понадобился" - это и есть сигнал
перегенерировать.

ЗАПУСК:
    python update_patch.py <путь к code-terraform.exe> <locale_ru.json> [папка_игры]

Если папка игры не указана, берётся папка самого exe (там же, где лежит
Translator.dll при обычной установке). Промежуточные ассеты складываются
в подпапку .assets_cache рядом с locale_ru.json и переиспользуются, если
exe не менялся.

Ключи: --pseudo, --tag, --check - те же, что у make_locale_patch.py.
"""

import hashlib
import os
import shutil
import subprocess
import sys

# Русский текст в выводе + чужая кодировка консоли = UnicodeEncodeError
# на ровном месте (подробности в _console.py).
from _console import enable_utf8_output

enable_utf8_output()

HERE = os.path.dirname(os.path.abspath(__file__))


def exe_fingerprint(path):
    """Хеш по размеру + началу/концу файла: читать 300 МБ целиком ради
    определения "поменялся ли exe" незачем, а подделать это случайно
    невозможно - при обновлении меняется и размер, и содержимое."""
    size = os.path.getsize(path)
    h = hashlib.sha1(str(size).encode())
    with open(path, "rb") as f:
        h.update(f.read(1 << 20))
        f.seek(max(0, size - (1 << 20)))
        h.update(f.read(1 << 20))
    return h.hexdigest()[:16]


def run(script, args):
    sys.stdout.flush()
    result = subprocess.run([sys.executable, os.path.join(HERE, script)] + args)
    if result.returncode != 0:
        raise SystemExit(f"[!] {script} завершился с кодом {result.returncode}")


def main():
    args = sys.argv[1:]
    passthrough = [a for a in args if a.startswith("--")]
    # У --tag и --check есть значение, его тоже нужно передать дальше.
    for flag in ("--tag", "--check"):
        if flag in args:
            passthrough.append(args[args.index(flag) + 1])
    positional = []
    skip = False
    for i, a in enumerate(args):
        if skip:
            skip = False
            continue
        if a in ("--tag", "--check"):
            skip = True
            continue
        if a.startswith("--"):
            continue
        positional.append(a)

    if len(positional) < 2:
        print(__doc__)
        return 1

    exe_path = os.path.abspath(positional[0])
    locale_file = os.path.abspath(positional[1])
    game_dir = os.path.abspath(positional[2]) if len(positional) > 2 \
        else os.path.dirname(exe_path)

    for path in (exe_path, locale_file):
        if not os.path.isfile(path):
            raise SystemExit(f"[!] Файл не найден: {path}")

    cache_root = os.path.join(os.path.dirname(locale_file), ".assets_cache")
    assets_dir = os.path.join(cache_root, exe_fingerprint(exe_path))

    if os.path.isdir(assets_dir) and os.listdir(assets_dir):
        print(f"[*] Ассеты этой версии игры уже распакованы: {assets_dir}")
    else:
        print("[*] Распаковываю ассеты из exe (это занимает полминуты)...")
        run("extract_assets.py", [exe_path, assets_dir])

    patch_dir = os.path.join(game_dir, "locale_patch")
    before = set()
    if os.path.isdir(patch_dir):
        for root, _dirs, files in os.walk(patch_dir):
            for name in files:
                before.add(os.path.join(root, name))

    print(f"[*] Собираю патч в {patch_dir}", flush=True)
    run("make_locale_patch.py", [assets_dir, locale_file, patch_dir] + passthrough)

    # Файлы от прошлой версии игры удаляем: они всё равно уже не совпадут с
    # тем, что запрашивает игра, а место занимают (там мегабайты).
    after = set()
    for root, _dirs, files in os.walk(patch_dir):
        for name in files:
            after.add(os.path.join(root, name))
    stale = before - after
    for path in sorted(stale):
        os.remove(path)
        print(f"[-] Удалён файл от прошлой версии игры: {os.path.relpath(path, patch_dir)}")

    # Кэш ассетов от прошлых версий тоже чистим - каждая копия весит ~300 МБ.
    for name in sorted(os.listdir(cache_root)):
        old = os.path.join(cache_root, name)
        if os.path.isdir(old) and old != assets_dir:
            shutil.rmtree(old, ignore_errors=True)
            print(f"[-] Удалены ассеты прошлой версии игры: {name}")

    print(f"\n[+] Готово. Перезапустите игру - в translator_log.txt должно появиться "
          f"'[assets] Подменён файл игры: ...'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
