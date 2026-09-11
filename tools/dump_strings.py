"""
Собирает ВСЕ строки интерфейса игры из уже извлечённых ассетов
(см. extract_assets.py) в готовую заготовку словаря для переводчика.

ГЛАВНОЕ, ЧТО СТОИТ ЗНАТЬ. В игре есть полноценная система локализации:
строки лежат не по коду вперемешку, а в одном каталоге вида

    { "computer": { "playground_ready": "Ready.", ... }, "console": {...}, ... }

а код зовёт их по ключу (H("computer.playground_ready")). Каталог целиком
вшит в один из js-чанков. Значит, ничего "выцарапывать" из экрана больше не
нужно: этот скрипт находит каталог, разбирает его и выдаёт полный список
строк - включая те, что на экране показываются редко (ошибки, подсказки,
разделы помощи).

Каталог опознаётся по форме (её же проверяет и сам код игры): объект, у
которого каждое значение - объект, а у того каждое значение - строка. Это
не зависит от имён переменных после минификации, поэтому скрипт переживёт
обновление игры.

ЧТО НА ВЫХОДЕ (в папке назначения):
  catalog.json                 - каталог как есть (группы -> ключи -> текст)
  catalog.flat.json            - плоско: "группа.ключ" -> текст
  translations.skeleton.json   - заготовка для translations.json:
                                 {"English text": "English text"} - правьте
                                 правую часть на русский
  strings_with_placeholders.txt- строки с подстановками ({name}, {count}):
                                 точным словарём они не ловятся, вынесены
                                 отдельно, чтобы не мусорить в заготовке

ЗАПУСК:
    python dump_strings.py <папка_с_ассетами> [папка_назначения]
    python dump_strings.py <папка_с_ассетами> --merge <translations.json>

--merge дописывает в существующий словарь только НОВЫЕ строки (уже
переведённое не трогает), чтобы после обновления игры не переводить всё
заново.
"""

import json
import os
import re
import sys

# Русский текст в выводе + чужая кодировка консоли = UnicodeEncodeError
# на ровном месте (подробности в _console.py).
from _console import enable_utf8_output

enable_utf8_output()


MIN_GROUPS = 20          # каталог интерфейса заведомо больше
SPREAD_KEY = "\x00spread"  # служебный ключ: список спредов внутри объекта
PLACEHOLDER = re.compile(r"\{[a-zA-Z_][a-zA-Z0-9_]*\}")

ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f", "v": "\v",
    "0": "\0", "\\": "\\", "'": "'", '"': '"', "`": "`", "/": "/",
    "\n": "",            # перенос строки после \ - это склейка строки
}


class ParseError(Exception):
    pass


class Ref:
    """Ссылка на другую переменную бандла.

    Каталог собран не одним куском: часть групп вынесена в отдельные
    объекты и подставлена по имени (api_reference:sn, diagnostics:yt).
    Такие значения запоминаем как ссылку и разрешаем вторым проходом -
    иначе разбор каталога обрывается на первой же из них.
    """

    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return f"Ref({self.name})"


def skip_ws(src, i):
    while i < len(src) and src[i] in " \t\r\n":
        i += 1
    return i


def parse_string(src, i):
    quote = src[i]
    i += 1
    out = []
    while i < len(src):
        c = src[i]
        if c == "\\":
            i += 1
            if i >= len(src):
                raise ParseError("обрыв на escape-последовательности")
            e = src[i]
            if e == "u":
                if src[i + 1] == "{":
                    end = src.index("}", i)
                    out.append(chr(int(src[i + 2:end], 16)))
                    i = end + 1
                    continue
                out.append(chr(int(src[i + 1:i + 5], 16)))
                i += 5
                continue
            if e == "x":
                out.append(chr(int(src[i + 1:i + 3], 16)))
                i += 3
                continue
            out.append(ESCAPES.get(e, e))
            i += 1
            continue
        if c == quote:
            return "".join(out), i + 1
        if c == "$" and quote == "`" and src[i + 1:i + 2] == "{":
            # Шаблонная подстановка ${...} - это уже не константа, каталогу
            # такое не свойственно: значит, мы разбираем не каталог.
            raise ParseError("шаблонная подстановка в строке")
        out.append(c)
        i += 1
    raise ParseError("незакрытая строка")


def parse_key(src, i):
    if src[i] in "'\"`":
        return parse_string(src, i)
    m = re.match(r"[A-Za-z_$][A-Za-z0-9_$]*", src[i:])
    if not m:
        raise ParseError("нераспознанный ключ")
    return m.group(), i + m.end()


def parse_spread(src, i):
    """Пропускает `...что-то` внутри объекта.

    Часть групп каталога дописывается спредом: `{...Hd(), feedback:{...}}`.
    Спред простой переменной (`...Hd`) можно разрешить, спред вызова
    (`...Hd()`) - нет, он вычисляется только в рантайме; такое просто
    пропускаем, не роняя разбор всего каталога.
    """
    i += 3
    m = re.match(r"[A-Za-z_$][A-Za-z0-9_$]*", src[i:])
    if not m:
        raise ParseError("непонятный спред")
    i += m.end()
    if src[i:i + 1] not in "(.":
        return Ref(m.group()), i

    # Вызов или обращение к свойству - пропускаем до конца выражения,
    # аккуратно считая скобки (внутри аргументов могут быть свои).
    depth = 0
    while i < len(src):
        c = src[i]
        if c in "'\"`":
            _, i = parse_string(src, i)
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif depth == 0 and c in ",}":
            return None, i
        i += 1
    raise ParseError("незакрытый спред")


def parse_object(src, i, depth=0):
    """Разбирает литерал JS-объекта. Понимает только то, из чего может
    состоять каталог строк: вложенные объекты, строковые литералы, ссылки
    на другие переменные бандла и спреды."""
    if depth > 4:
        raise ParseError("слишком глубокая вложенность для каталога")
    if src[i] != "{":
        raise ParseError("ожидался {")
    i += 1
    obj = {}
    while True:
        i = skip_ws(src, i)
        if i >= len(src):
            raise ParseError("незакрытый объект")
        if src[i] == "}":
            return obj, i + 1
        if src.startswith("...", i):
            spread, i = parse_spread(src, i)
            if spread is not None:
                obj.setdefault(SPREAD_KEY, []).append(spread)
            i = skip_ws(src, i)
            if i < len(src) and src[i] == ",":
                i += 1
            continue
        key, i = parse_key(src, i)
        i = skip_ws(src, i)
        if src[i] != ":":
            raise ParseError("ожидалось : после ключа")
        i = skip_ws(src, i + 1)
        if src[i] == "{":
            value, i = parse_object(src, i, depth + 1)
        elif src[i] in "'\"`":
            value, i = parse_string(src, i)
        else:
            m = re.match(r"[A-Za-z_$][A-Za-z0-9_$]*", src[i:])
            # Голое имя переменной - ссылка на вынесенную часть каталога.
            # Всё остальное (вызовы, массивы, числа) для каталога чужеродно.
            if not m or src[i + m.end():i + m.end() + 1] not in ",}":
                raise ParseError("значение не строка, не объект и не ссылка")
            value, i = Ref(m.group()), i + m.end()
        obj[key] = value
        i = skip_ws(src, i)
        if i < len(src) and src[i] == ",":
            i += 1


def resolve_refs(obj, src, cache, depth=0):
    """Подставляет вместо Ref содержимое соответствующей переменной бандла.

    Возвращает (значение, сколько ссылок разрешить не удалось). Неразрешимые
    ветки выбрасываются: часть групп игра собирает уже в рантайме
    (Object.fromEntries(...)), их из статики достать нельзя.
    """
    if isinstance(obj, str):
        return obj, 0
    if isinstance(obj, Ref):
        if depth > 3 or obj.name in cache and cache[obj.name] is None:
            return None, 1
        if obj.name not in cache:
            cache[obj.name] = None            # защита от циклов
            target = find_assignment(src, obj.name)
            cache[obj.name] = target
        target = cache[obj.name]
        if target is None:
            return None, 1
        return resolve_refs(target, src, cache, depth + 1)

    out, unresolved = {}, 0
    # Спреды раскрываем первыми: явно заданные ключи должны их перекрывать,
    # как это и работает в JS.
    for spread in obj.get(SPREAD_KEY, []):
        resolved, misses = resolve_refs(spread, src, cache, depth + 1)
        unresolved += misses
        if isinstance(resolved, dict):
            out.update(resolved)
    for key, value in obj.items():
        if key == SPREAD_KEY:
            continue
        resolved, misses = resolve_refs(value, src, cache, depth)
        unresolved += misses
        if resolved is not None:
            out[key] = resolved
    return out, unresolved


def find_assignment(src, name):
    """Находит IDENT={...} в бандле и разбирает объект."""
    for m in re.finditer(r"(?<![\w$.])" + re.escape(name) + r"\s*=\s*\{", src):
        start = m.end() - 1
        try:
            obj, _ = parse_object(src, start)
        except (ParseError, ValueError, IndexError):
            continue
        if obj:
            return obj
    return None


def catalog_score(obj):
    """Насколько объект похож на каталог локализации.

    Проверка - та же, что и в самой игре при загрузке локали: объект групп,
    внутри каждой группы только строки. Возвращает число строк (0 - не
    каталог), чтобы из нескольких кандидатов выбрать самый полный.
    """
    if not isinstance(obj, dict) or len(obj) < MIN_GROUPS:
        return 0
    total = 0
    for group in obj.values():
        if not isinstance(group, dict) or not group:
            return 0
        if not all(isinstance(v, str) for v in group.values()):
            return 0
        total += len(group)
    return total


def find_catalog(src):
    """Ищет в js-чанке присваивание вида IDENT={группа:{ключ:`текст`}...}."""
    best, best_score, best_misses = None, 0, 0
    cache = {}
    for m in re.finditer(r"[=,;(\[]\s*\{(?=[A-Za-z_$\"'`])", src):
        start = m.end() - 1
        # Быстрый отсев: у каталога сразу после { идёт "имя_группы:{".
        head = src[start:start + 64]
        if not re.match(r"\{[A-Za-z_$][A-Za-z0-9_$]*\s*:\s*\{", head):
            continue
        try:
            obj, _ = parse_object(src, start)
        except (ParseError, ValueError, IndexError):
            continue
        obj, misses = resolve_refs(obj, src, cache)
        score = catalog_score(obj)
        if score > best_score:
            best, best_score, best_misses = obj, score, misses
    return best, best_misses


def flatten(catalog):
    flat = {}
    for group, entries in catalog.items():
        for key, text in entries.items():
            flat[f"{group}.{key}"] = text
    return flat


def main():
    args = [a for a in sys.argv[1:]]
    if not args:
        print(__doc__)
        return 1

    assets_dir = args[0]
    merge_into = None
    out_dir = None
    if "--merge" in args:
        merge_into = args[args.index("--merge") + 1]
    elif len(args) > 1:
        out_dir = args[1]
    if out_dir is None:
        out_dir = assets_dir

    js_files = []
    for root, _dirs, files in os.walk(assets_dir):
        for name in files:
            if name.endswith((".js", ".mjs")):
                js_files.append(os.path.join(root, name))
    # Каталог лежит в одном из крупных чанков - начинаем с них, чтобы не
    # перемалывать сначала мелочь.
    js_files.sort(key=lambda p: -os.path.getsize(p))
    print(f"[*] js-файлов для разбора: {len(js_files)}")

    catalog = None
    for path in js_files:
        src = open(path, encoding="utf-8", errors="replace").read()
        found, misses = find_catalog(src)
        if found:
            catalog = found
            print(f"[+] Каталог строк найден: {os.path.basename(path)} "
                  f"({len(found)} групп)")
            if misses:
                print(f"    групп, собираемых игрой только в рантайме "
                      f"(в дамп не попадут): {misses}")
            break

    if catalog is None:
        print("[!] Каталог строк не найден. Возможно, игра обновилась и "
              "формат изменился - тогда остаётся дамп с экрана (F9 в игре).")
        return 2

    flat = flatten(catalog)
    plain, with_placeholders = {}, []
    for key, text in sorted(flat.items()):
        text = text.strip()
        if not text:
            continue
        if PLACEHOLDER.search(text):
            with_placeholders.append(f"{key}\t{text}")
        else:
            plain.setdefault(text, text)

    os.makedirs(out_dir, exist_ok=True)

    def write_json(name, data):
        path = os.path.join(out_dir, name)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)
        print(f"    {path}")

    print(f"[*] Строк всего: {len(flat)}, из них без подстановок: {len(plain)}")
    write_json("catalog.json", catalog)
    write_json("catalog.flat.json", flat)

    if merge_into:
        try:
            existing = json.load(open(merge_into, encoding="utf-8"))
        except (OSError, ValueError):
            existing = {}
        # Поддерживаем оба формата translations.json (плоский и с settings).
        target = existing["translations"] if isinstance(existing.get("translations"), dict) else existing
        added = 0
        for text in plain:
            if text not in target:
                target[text] = text
                added += 1
        with open(merge_into, "w", encoding="utf-8") as f:
            json.dump(existing, f, ensure_ascii=False, indent=2, sort_keys=True)
        print(f"[+] В {merge_into} добавлено новых строк: {added} "
              f"(уже переведённые не тронуты)")
    else:
        write_json("translations.skeleton.json", plain)

    path = os.path.join(out_dir, "strings_with_placeholders.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write("# Строки с подстановками ({name}, {count}) - точным словарём\n"
                "# не ловятся, тут они справочно, с ключами каталога.\n\n")
        f.write("\n".join(with_placeholders))
    print(f"    {path} ({len(with_placeholders)} строк)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
