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

# Копируем translations.json рядом со скомпилированной DLL, чтобы
# Translate::Reload() сразу мог его найти.
add_custom_command(TARGET Translator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/translations.json"
        "$<TARGET_FILE_DIR:Translator>/translations.json"
)
