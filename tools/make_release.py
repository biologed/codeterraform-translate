"""
Собирает архив для игроков и считает SHA-256.

ЗАЧЕМ СЧИТАТЬ ХЕШИ. Мод внедряет DLL в чужой процесс, и антивирусы регулярно
поднимают на него ложную тревогу. Единственное, что можно этому
противопоставить без сертификата за деньги, - проверяемость: пользователь
должен иметь возможность убедиться, что скачанный файл в точности тот, что
выложил автор, и что собран он из открытого исходного кода. Поэтому рядом с
архивом всегда кладётся SHA256SUMS.txt, а сами сборки делает CI
(см. .github/workflows/build.yml).

ЗАПУСК (из корня проекта, после cmake --build):
    python tools/make_release.py [папка_сборки] [папка_назначения]

По умолчанию: build -> release/

На выходе всё лежит ПЛОСКО, без вложенных папок - ровно так, как это потом
окажется в папке игры:

    release/CodeTerraform-RU/     <- содержимое архива, можно копировать как есть
        Translator.dll
        TranslatorLauncher.exe
        locale_ru.json
        Запустить с переводом.bat
        README.txt
        SHA256SUMS.txt
    release/CodeTerraform-RU.zip  <- то же самое архивом, для GitHub Releases
"""

import hashlib
import os
import shutil
import sys
import zipfile

# Русский текст в выводе + чужая кодировка консоли = UnicodeEncodeError
# на ровном месте (подробности в _console.py).
from _console import enable_utf8_output

enable_utf8_output()

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCHIVE_NAME = "CodeTerraform-RU"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "release")

    # Собранное лежит в одной папке build/bin (см. корневой CMakeLists.txt).
    # Старую раскладку по подпапкам тоже проверяем - на случай каталога
    # сборки, созданного до этого изменения.
    def built(name, *old_parts):
        candidates = [os.path.join(build_dir, "bin", name),
                      os.path.join(build_dir, *old_parts, name)]
        return next((p for p in candidates if os.path.isfile(p)), candidates[0])

    items = [
        (built("Translator.dll", "dll", "Release"), "Translator.dll"),
        (built("TranslatorLauncher.exe", "injector", "Release"), "TranslatorLauncher.exe"),
        (os.path.join(ROOT, "locale", "locale_ru.json"), "locale_ru.json"),
        (os.path.join(ROOT, "install", "Запустить с переводом.bat"), "Запустить с переводом.bat"),
        (os.path.join(ROOT, "install", "README.txt"), "README.txt"),
    ]

    missing = [src for src, _ in items if not os.path.isfile(src)]
    if missing:
        print("[!] Не найдены файлы (соберите проект: cmake --build build --config Release):")
        for path in missing:
            print("   ", path)
        return 1

    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    # Имя папки = имя архива: при распаковке "в текущую папку" получится
    # ровно один каталог с понятным названием, а не мусор из пяти файлов.
    payload_dir = os.path.join(out_dir, ARCHIVE_NAME)
    os.makedirs(payload_dir)

    sums = []
    for src, name in items:
        shutil.copy2(src, os.path.join(payload_dir, name))
        digest = sha256(src)
        sums.append(f"{digest}  {name}")
        print(f"[+] {name:32} {os.path.getsize(src):>9,} байт  {digest[:16]}...")

    sums_path = os.path.join(payload_dir, "SHA256SUMS.txt")
    with open(sums_path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(sums) + "\n")

    archive = os.path.join(out_dir, ARCHIVE_NAME + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for _src, name in items:
            zf.write(os.path.join(payload_dir, name), name)
        zf.write(sums_path, "SHA256SUMS.txt")

    # Папка и архив содержат одно и то же: папку удобно просто скопировать в
    # каталог игры, архив - выложить в релиз.
    print(f"\n[+] Папка: {payload_dir}")
    print(f"[+] Архив: {archive}")
    print("\nВ описание релиза имеет смысл вынести:")
    print("  * эти SHA-256 (чтобы их можно было сверить, не распаковывая архив);")
    print("  * строку про ложные срабатывания антивирусов и ссылку на исходники;")
    print("  * команду проверки происхождения сборки:")
    print("      gh attestation verify Translator.dll -R <владелец>/<репозиторий>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
