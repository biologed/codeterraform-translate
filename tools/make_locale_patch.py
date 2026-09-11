"""
Готовит подменные js-файлы игры с русской локалью.

ЗАЧЕМ ЭТО ЛУЧШЕ ПЕРЕВОДА ПО DOM. В игре уже есть система локализации:
строки лежат каталогом {группа: {ключ: текст}}, а код зовёт их по ключу.
Если добавить в эту систему русский каталог, перевод произойдёт ВНУТРИ игры,
до того как текст вообще попадёт на страницу. Отсюда сразу:

  * текст игрока (код в редакторе, вывод его программ) не подменяется в
    принципе - мы не трогаем DOM, а переводим по ключам;
  * работают строки с подстановками ("Урон: {n}") - они собираются уже из
    русского шаблона;
  * ключи, для которых перевода ещё нет, сами показываются по-английски -
    в игре это штатный фолбэк, никакой отдельной обработки не нужно.

ЧТО ДЕЛАЕТ СКРИПТ. Функция инициализации локалей выглядит так (после
минификации имена другие, но форма одна и та же):

    function init(catalogs, lang=`en`) {
        MAP1.clear(), MAP2.clear(), MAP3.clear();
        for (let [tag, cat] of Object.entries(catalogs))
            MAP1.set(tag, cat), MAP2.set(tag, flatten(cat));
        ACTIVE = lang, applyDir(ACTIVE), log.info(`I18N`, ...)
    }

Скрипт находит её по форме (а не по именам - те меняются при каждой сборке
игры), дописывает рядом наш каталог и вставляет в тело функции регистрацию
нашего языка + переключение на него. Патч кладётся отдельными файлами,
файлы игры не трогаются - подменяет их на лету DLL (см. assets.cpp).

Копий этой функции в сборке несколько: общий модуль i18n-*.js для окна игры
и по копии внутри воркеров. Патчатся все, у кого совпала форма.

ЗАПУСК:
    python make_locale_patch.py <папка_ассетов> <locale_ru.json> [папка_патча]

  <папка_ассетов>  - вывод extract_assets.py
  <locale_ru.json> - каталог перевода в формате игры: {"группа": {"ключ": "текст"}}
                     Заготовку берите из catalog.json (вывод dump_strings.py):
                     это тот же файл с английскими значениями.
  [папка_патча]    - куда сложить подменные файлы; по умолчанию
                     <папка_ассетов>/../locale_patch. ЭТУ ПАПКУ НУЖНО
                     ПОЛОЖИТЬ РЯДОМ С Translator.dll.

Полезные ключи:
    --tag ru          код языка (по умолчанию ru)
    --check FILE      сверить ключи с английским каталогом (catalog.json)
                      и показать те, которых в игре нет - обычно опечатки
    --pseudo          не перевод, а проверка связи: каждая строка игры
                      показывается как «текст» в кавычках-ёлочках. Если
                      после этого интерфейс в ёлочках - подмена работает.
"""

import json
import os
import re
import sys

# Русский текст в выводе + чужая кодировка консоли = UnicodeEncodeError
# на ровном месте (подробности в _console.py).
from _console import enable_utf8_output

enable_utf8_output()


# Форма функции инициализации локалей. Имена переменных после минификации
# произвольные, поэтому ловим структуру, а сами имена забираем группами:
#   1 - имя функции, 2 - параметр с каталогами, 3 - параметр с языком,
#   4 - map "тег -> каталог", 5 - map "тег -> плоский каталог",
#   6 - map кэша, 7/8 - переменные цикла, 9 - функция flatten,
#   10 - переменная активного языка.
INIT_RE = re.compile(
    r"function\s+(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\s*=\s*`en`\s*\)\s*\{"
    r"\s*(\w+)\.clear\(\)\s*,\s*(\w+)\.clear\(\)\s*,\s*(\w+)\.clear\(\)\s*;"
    r"\s*for\s*\(\s*let\s*\[\s*(\w+)\s*,\s*(\w+)\s*\]\s*of\s*Object\.entries\(\2\)\)"
    r"\s*\4\.set\(\7\s*,\s*\8\)\s*,\s*\5\.set\(\7\s*,\s*(\w+)\(\8\)\)\s*;"
    r"\s*(\w+)\s*=\s*\3\s*,"
)

# ГЛАВНАЯ точка внедрения - функция поиска перевода:
#
#     function w(key, lang) { let m = MAP.get(lang); return m ? m.get(key) ?? null : null }
#
# Почему именно она, а не инициализация локалей. Оказалось, что игра
# инициализацию НЕ вызывает вовсе: английский текст лежит прямо в коде
# рядом с ключами и подставляется как запасной вариант, если перевод не
# нашёлся. То есть система локализации работает "наоборот" - не "загрузили
# язык и показали", а "спросили перевод, не нашли - показали исходник".
# Поэтому патч инициализации ничего не менял (в логе было localeApplied:null).
#
# Зато ЧЕРЕЗ ЭТУ функцию проходят все пути перевода без исключения: и
# обычный t(ключ), и вариант с исходником в коде, и множественные числа.
# Отдав из неё свою строку, мы переводим игру независимо от того, была ли
# вообще выбрана локаль.
LOOKUP_RE = re.compile(
    r"function\s+(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*\{"
    r"\s*let\s+(\w+)\s*=\s*(\w+)\.get\(\3\)\s*;"
    r"\s*return\s+\4\s*\?\s*\4\.get\(\2\)\s*\?\?\s*null\s*:\s*null\s*\}"
)


def validate_catalog(catalog):
    """Та же проверка, что делает сама игра: объект групп, внутри строки."""
    if not isinstance(catalog, dict) or not catalog:
        raise SystemExit("Каталог должен быть непустым объектом "
                         '{"группа": {"ключ": "текст"}}')
    for group, entries in catalog.items():
        if not isinstance(entries, dict):
            raise SystemExit(f"Группа {group!r} должна быть объектом")
        for key, text in entries.items():
            if not isinstance(text, str):
                raise SystemExit(f"Значение {group}.{key} должно быть строкой")


def strip_untranslated(catalog):
    """Выкидывает пустые строки: пустой перевод - это не перевод, пусть
    игра сама покажет английский оригинал (у неё штатный фолбэк)."""
    out, dropped = {}, 0
    for group, entries in catalog.items():
        kept = {k: v for k, v in entries.items() if v.strip()}
        dropped += len(entries) - len(kept)
        if kept:
            out[group] = kept
    return out, dropped


def make_pseudo(english):
    return {g: {k: f"«{v}»" for k, v in entries.items()}
            for g, entries in english.items()}


def lookup_injection(key_arg):
    """Тело врезки в функцию поиска перевода.

    Отметку об успехе ставим ОДИН раз, а не на каждый ключ: через эту
    функцию проходят тысячи обращений за кадр, и запись в глобальный объект
    на каждом из них была бы заметна.
    """
    return (
        f"var __trV=typeof __TR_FLAT!=='undefined'?__TR_FLAT[{key_arg}]:undefined;"
        "if(__trV!==undefined){"
        "if(!__trMarked){__trMarked=1;try{(typeof globalThis!=='undefined'?globalThis:this)"
        ".__TR_LOCALE_APPLIED={keys:__TR_KEYS,at:new Date().toISOString()};}catch(e){}}"
        "return __trV;}"
    )


def init_injection(m):
    """Тело врезки в функцию инициализации локалей.

    Строго говоря, без неё перевод уже работает (всё делает врезка в поиск).
    Но если игра всё же вызовет инициализацию - например, когда в неё
    завезут переключатель языка, - пусть наш язык станет активным по-честному:
    тогда заодно включатся Intl-форматы (числа, даты) и правила
    множественного числа нужного языка.
    """
    map_raw, map_flat, flatten, active = m.group(4), m.group(5), m.group(9), m.group(10)
    return (
        "(function(){var G=typeof globalThis!=='undefined'?globalThis:this;try{"
        "if(typeof __TR_LOCALE==='undefined'||!__TR_LOCALE||!__TR_LOCALE.catalog)return;"
        f"{map_raw}.set(__TR_LOCALE.tag,__TR_LOCALE.catalog);"
        f"{map_flat}.set(__TR_LOCALE.tag,{flatten}(__TR_LOCALE.catalog));"
        f"{active}=__TR_LOCALE.tag;"
        "G.__TR_LOCALE_INIT=__TR_LOCALE.tag;"
        "}catch(e){G.__TR_LOCALE_ERROR=String(e&&e.message||e);}})(),"
    )


def patch_source(src, decls):
    """Патчит обе точки внедрения. Возвращает (текст, [что пропатчено])."""
    lookup = LOOKUP_RE.search(src)
    init = INIT_RE.search(src)
    if not lookup and not init:
        return None, []

    edits, applied = [], []
    if lookup:
        body_start = src.index("{", lookup.start()) + 1
        edits.append((body_start, lookup_injection(lookup.group(2))))
        applied.append("поиск перевода")
    if init:
        edits.append((init.end(), init_injection(init)))
        applied.append("инициализация локалей")

    # Объявления ставим перед самой ранней из пропатченных функций - так они
    # гарантированно оказываются в той же области видимости, что и врезки, и
    # это одинаково работает в модуле окна игры и внутри воркера (где нет
    # никакого window).
    edits.append((min(m.start() for m in (lookup, init) if m), decls))

    # Вставляем с конца, чтобы более ранние смещения не поехали.
    for offset, text in sorted(edits, key=lambda e: -e[0]):
        src = src[:offset] + text + src[offset:]
    return src, applied


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 1

    tag = "ru"
    if "--tag" in args:
        i = args.index("--tag")
        tag = args[i + 1]
        del args[i:i + 2]

    check_file = None
    if "--check" in args:
        i = args.index("--check")
        check_file = args[i + 1]
        del args[i:i + 2]

    pseudo = "--pseudo" in args
    if pseudo:
        args.remove("--pseudo")

    assets_dir = args[0]
    locale_file = args[1]
    patch_dir = args[2] if len(args) > 2 else \
        os.path.join(os.path.dirname(os.path.abspath(assets_dir)), "locale_patch")

    catalog = json.load(open(locale_file, encoding="utf-8"))
    validate_catalog(catalog)
    if pseudo:
        catalog = make_pseudo(catalog)
        print("[*] Режим проверки связи: каждая строка будет в «ёлочках»")
    catalog, dropped = strip_untranslated(catalog)
    strings = sum(len(v) for v in catalog.values())
    print(f"[*] Каталог '{tag}': групп {len(catalog)}, строк {strings}"
          + (f" (пустых пропущено: {dropped})" if dropped else ""))

    if check_file:
        english = json.load(open(check_file, encoding="utf-8"))
        unknown = [f"{g}.{k}" for g, entries in catalog.items()
                   for k in entries if k not in english.get(g, {})]
        if unknown:
            print(f"[!] Ключей, которых нет в английском каталоге: {len(unknown)}")
            for key in unknown[:15]:
                print("   ", key)
            if len(unknown) > 15:
                print(f"    ... и ещё {len(unknown) - 15}")
        else:
            print("[+] Все ключи нашлись в английском каталоге")

    # Плоский вид "группа.ключ" -> текст: именно такими ключами игра ищет
    # перевод (её собственный flatten делает ровно это же).
    flat = {f"{group}.{key}": text
            for group, entries in catalog.items()
            for key, text in entries.items()}

    def literal(value):
        # U+2028/2029 внутри JS-строк исторически ломали парсеры; заменяем на
        # экранированную форму - в JSON это тот же символ.
        return (json.dumps(value, ensure_ascii=False)
                .replace("\u2028", "\\u2028").replace("\u2029", "\\u2029"))

    # Безусловная отметка "наш файл исполнился". Без неё невозможно отличить
    # "патч не доехал до страницы" от "доехал, но врезка не сработала" -
    # а это принципиально разные причины с разным лечением.
    decls = ("try{(typeof globalThis!=='undefined'?globalThis:this).__TR_PATCH_LOADED="
             "((typeof globalThis!=='undefined'?globalThis:this).__TR_PATCH_LOADED||0)+1;}"
             "catch(e){}"
             "var __TR_LOCALE=" + literal({"tag": tag, "catalog": catalog}) + ","
             "__TR_FLAT=" + literal(flat) + ","
             f"__TR_KEYS={len(flat)},__trMarked=0;")

    if not os.path.isdir(assets_dir):
        raise SystemExit(
            f"[!] Папки с ассетами нет: {assets_dir}\n"
            f"    Это должна быть распакованная копия фронтенда игры (вывод\n"
            f"    extract_assets.py). Проще всего не следить за ней вручную, а\n"
            f"    запускать update_patch.py - он сам распакует ассеты из exe\n"
            f"    и закэширует их.")

    patched_files = 0
    scanned_js = 0
    for root, _dirs, files in os.walk(assets_dir):
        for name in sorted(files):
            if not name.endswith((".js", ".mjs")):
                continue
            scanned_js += 1
            path = os.path.join(root, name)
            src = open(path, encoding="utf-8", errors="replace").read()
            if "??null:null}" not in src and "`en`" not in src:   # дешёвый отсев
                continue
            patched, applied = patch_source(src, decls)
            if patched is None:
                continue

            rel = os.path.relpath(path, assets_dir)
            dest = os.path.join(patch_dir, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "w", encoding="utf-8", newline="") as f:
                f.write(patched)
            patched_files += 1
            print(f"[+] {rel}: пропатчено ({', '.join(applied)}), "
                  f"{len(patched):,} байт")

    if not patched_files:
        # Два совершенно разных случая, и путать их нельзя: чаще всего дело
        # не в игре, а в том, что указали не ту папку (или ассеты удалили).
        if scanned_js == 0:
            print(f"[!] В {assets_dir} нет ни одного js-файла - похоже, это не папка "
                  f"с ассетами игры.\n"
                  f"    Нужна распакованная копия фронтенда (вывод extract_assets.py); "
                  f"проще всего запустить update_patch.py, он сделает это сам.")
        else:
            print(f"[!] Просмотрено js-файлов: {scanned_js}, но ни функция поиска "
                  f"перевода, ни функция инициализации локалей не нашлись.\n"
                  f"    Скорее всего игра обновилась и их форма изменилась - "
                  f"смотрите LOOKUP_RE и INIT_RE в этом скрипте.")
        return 2

    print(f"\n[+] Готово: {patched_files} файл(ов) в {patch_dir}")
    print("    Положите эту папку рядом с Translator.dll - DLL подменит ими "
          "файлы игры на лету.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
