# Собирает Translator.dll.
#
# Проще всего собрать через vcpkg (manifest mode уже настроен файлом
# ../vcpkg.json). Пример из папки Translator/:
#
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<путь_к_vcpkg>/scripts/buildsystems/vcpkg.cmake -A x64
#   cmake --build build --config Release
#
# ВАЖНО: -A x64 (или x86) должен совпадать с битностью игры!

add_library(Translator SHARED
    dllmain.cpp
    hooks.cpp
    hooks.h
    webview.cpp
    webview.h
    assets.cpp
    assets.h
    gameassets.cpp
    gameassets.h
    locale.cpp
    locale.h
    translate.cpp
    translate.h
)

target_compile_features(Translator PRIVATE cxx_std_17)

add_version_info(Translator
    "Модуль перевода Code: Terraform на русский"
    "Translator.dll"
    VFT_DLL)

find_package(minhook CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(unofficial-brotli CONFIG REQUIRED)   # распаковка ассетов, вшитых в exe игры

target_link_libraries(Translator PRIVATE
    minhook::minhook
    nlohmann_json::nlohmann_json
    unofficial::webview2::webview2
    unofficial::brotli::brotlidec
    Shlwapi.lib
)

# Заголовки WebView2.h/WebView2EnvironmentOptions.h приезжают из vcpkg-порта
# "webview2" (см. vcpkg.json) вместе с целью unofficial::webview2::webview2 -
# отдельно ставить NuGet-пакет Microsoft.Web.WebView2 не нужно.

# Кладём рядом со собранной DLL то, что она читает при старте: словарь для
# перевода по DOM и сам перевод. После сборки build/bin можно целиком копировать
# в папку игры - не хватит только лаунчера с батником, их добавляет
# tools/make_release.py.
#
# Файлы с русскими именами ("Запустить с переводом.bat", "ЧИТАЙ МЕНЯ.txt")
# здесь намеренно НЕ копируются: в цепочке Git Bash -> MSBuild -> CMake имя
# перекодируется и copy_if_different не находит файл, роняя всю сборку. Их
# кладёт в архив make_release.py, у Python с Unicode в путях проблем нет.
add_custom_command(TARGET Translator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/translations.json"
        "$<TARGET_FILE_DIR:Translator>/translations.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/locale/locale_ru.json"
        "$<TARGET_FILE_DIR:Translator>/locale_ru.json"
)
