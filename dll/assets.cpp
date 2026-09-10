#include "assets.h"
#include "hooks.h"
#include "webview.h"
#include "gameassets.h"
#include "locale.h"

#include <wrl.h>
#include <shlwapi.h>
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Microsoft::WRL;

namespace {

    // Окружение нужно ровно для одного: только оно умеет создавать
    // ICoreWebView2WebResourceResponse, который мы отдаём вместо файла игры.
    ComPtr<ICoreWebView2Environment> g_environment;

    std::wstring g_patchRoot;                 // <папка DLL>\locale_patch
    std::mutex g_logMutex;
    std::set<std::wstring> g_alreadyLogged;   // чтобы не писать в лог один и тот же файл сотни раз
    std::atomic<int> g_servedCount{0};        // сколько файлов реально подменили
    bool g_active = false;                    // включилась ли подмена вообще

    bool DirectoryExists(const std::wstring& path)
    {
        DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool FileExists(const std::wstring& path)
    {
        DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }

    // Рекурсивно собирает пути файлов патча ОТНОСИТЕЛЬНО его корня, в виде
    // "assets/i18n-Dh-M7E2V.js" - то есть ровно так, как игра эти файлы
    // запрашивает у себя же по http.
    void CollectPatchFiles(const std::wstring& dir, const std::wstring& prefix,
                           std::vector<std::wstring>& out)
    {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return;

        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                CollectPatchFiles(dir + L"\\" + name, prefix + name + L"/", out);
            } else {
                out.push_back(prefix + name);
            }
        } while (FindNextFileW(find, &fd));

        FindClose(find);
    }

    // "http://tauri.localhost/assets/x.js?v=1" -> "assets/x.js"
    std::wstring PathFromUri(const std::wstring& uri)
    {
        size_t schemeEnd = uri.find(L"://");
        if (schemeEnd == std::wstring::npos) return L"";
        size_t pathStart = uri.find(L'/', schemeEnd + 3);
        if (pathStart == std::wstring::npos) return L"";

        std::wstring path = uri.substr(pathStart + 1);
        size_t cut = path.find_first_of(L"?#");
        if (cut != std::wstring::npos) path = path.substr(0, cut);
        return path;
    }

    std::wstring ContentTypeFor(const std::wstring& path)
    {
        auto endsWith = [&path](const wchar_t* ext) {
            size_t len = wcslen(ext);
            return path.size() >= len && path.compare(path.size() - len, len, ext) == 0;
        };
        if (endsWith(L".js") || endsWith(L".mjs")) return L"text/javascript; charset=utf-8";
        if (endsWith(L".css")) return L"text/css; charset=utf-8";
        if (endsWith(L".html")) return L"text/html; charset=utf-8";
        if (endsWith(L".json")) return L"application/json; charset=utf-8";
        return L"application/octet-stream";
    }

    bool ReadWholeFile(const std::wstring& path, std::string& out)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        std::streamoff size = file.tellg();
        if (size < 0) return false;
        file.seekg(0);
        out.resize(static_cast<size_t>(size));
        if (!out.empty()) file.read(out.data(), size);
        return file.good() || file.eof();
    }

    // Уже пропатченные в памяти файлы и те, которые патчить бесполезно
    // (не тот чанк) - чтобы не распаковывать по мегабайту на каждый запрос.
    std::mutex g_cacheMutex;
    std::unordered_map<std::wstring, std::shared_ptr<const std::string>> g_patchedCache;
    std::set<std::wstring> g_notPatchable;

    void LogServed(const std::wstring& path, size_t size, const wchar_t* how)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_alreadyLogged.insert(path).second) {
            WriteLog(L"[assets] Подменён файл игры: " + path + L" (" + std::to_wstring(size)
                     + L" байт, " + how + L")");
        }
    }

    // ПОЧЕМУ ОТВЕТ ВЫСТАВЛЯЕТСЯ ОТЛОЖЕННО, А НЕ СРАЗУ.
    //
    // Свой обработчик этого же события есть и у самой игры: Tauri (wry)
    // обслуживает через него собственный протокол http://tauri.localhost и
    // отдаёт файлы, вшитые в exe. Обработчиков может быть сколько угодно, и
    // до страницы доходит ответ того, кто выставил его ПОСЛЕДНИМ. Прямая
    // запись этот спор проигрывала: в лог писалось "файл подменён", а
    // страница получала оригинал (видно по размеру: 4 677 байт вместо наших
    // мегабайт). Порядок регистрации обработчиков не помогает - игра ставит
    // свой позже в любом случае.
    //
    // Deferral - штатный способ сказать WebView2 "ответ будет позже": событие
    // не считается обработанным, пока мы не вызовем Complete(). Работу ставим
    // в очередь окна игры, поэтому она выполняется уже ПОСЛЕ всех её
    // обработчиков, и наш ответ оказывается последним по определению.
    //
    // Бонусом это решает и вторую задачу: тяжёлую распаковку с патчем можно
    // спокойно делать в фоновом потоке - страница просто подождёт.
    void RespondWith(ComPtr<ICoreWebView2WebResourceRequestedEventArgs> args,
                     ComPtr<ICoreWebView2Deferral> deferral,
                     std::shared_ptr<const std::string> content,
                     std::wstring path)
    {
        WebViewHooks::RunOnUiThread([args, deferral, content, path]() {
            if (content && g_environment) {
                ComPtr<IStream> stream;
                stream.Attach(SHCreateMemStream(
                    reinterpret_cast<const BYTE*>(content->data()),
                    static_cast<UINT>(content->size())));

                ComPtr<ICoreWebView2WebResourceResponse> response;
                std::wstring headers = L"Content-Type: " + ContentTypeFor(path)
                    + L"\r\nCache-Control: no-store"
                    // Ассеты игры лежат на её собственном origin, но воркеры и
                    // модули иногда тянутся с проверкой CORS - разрешаем явно,
                    // чтобы подменённый файл вёл себя ровно как оригинал.
                    + L"\r\nAccess-Control-Allow-Origin: *";

                if (stream && SUCCEEDED(g_environment->CreateWebResourceResponse(
                        stream.Get(), 200, L"OK", headers.c_str(), &response)) && response) {
                    args->put_Response(response.Get());
                    g_servedCount++;
                } else {
                    WriteLog(L"[assets] Не удалось собрать ответ для " + path
                             + L" - отдаём оригинал игры");
                }
            }
            if (deferral) deferral->Complete();
        });
    }

    bool IsJavaScript(const std::wstring& path)
    {
        return path.size() > 3 && (path.compare(path.size() - 3, 3, L".js") == 0
                                   || (path.size() > 4 && path.compare(path.size() - 4, 4, L".mjs") == 0));
    }

    HRESULT OnWebResourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args)
    {
        if (!g_environment) return S_OK;

        ComPtr<ICoreWebView2WebResourceRequest> request;
        if (FAILED(args->get_Request(&request)) || !request) return S_OK;

        LPWSTR rawUri = nullptr;
        if (FAILED(request->get_Uri(&rawUri)) || !rawUri) return S_OK;
        std::wstring uri(rawUri);
        CoTaskMemFree(rawUri);

        std::wstring path = PathFromUri(uri);
        // Ни ".." в пути, ни абсолютных путей: файл обязан лежать ВНУТРИ
        // locale_patch, что бы страница ни попросила.
        if (path.empty() || path.find(L"..") != std::wstring::npos
            || path.find(L':') != std::wstring::npos) {
            return S_OK;
        }

        // Решаем, интересен ли нам запрос, ДО того как брать deferral:
        // фильтр WebView2 широкий, а откладывать каждый запрос за шрифтом
        // ради того, чтобы тут же его отпустить, незачем.
        std::wstring filePath = g_patchRoot + L"\\" + path;
        for (auto& c : filePath) if (c == L'/') c = L'\\';
        const bool haveFile = !g_patchRoot.empty() && FileExists(filePath);
        const bool canAutoPatch = Locale::IsLoaded() && IsJavaScript(path);
        if (!haveFile && !canAutoPatch) return S_OK;

        ComPtr<ICoreWebView2WebResourceRequestedEventArgs> keepArgs(args);
        ComPtr<ICoreWebView2Deferral> deferral;
        args->GetDeferral(&deferral);

        // 1. Готовый файл из папки locale_patch, если она есть. Это ручной
        //    режим: подложить туда можно что угодно, он перебивает автопатч.
        if (haveFile) {
            auto content = std::make_shared<std::string>();
            if (ReadWholeFile(filePath, *content)) {
                LogServed(path, content->size(), L"файл из locale_patch");
                RespondWith(keepArgs, deferral, content, path);
                return S_OK;
            }
            WriteLog(L"[assets] Не удалось прочитать " + filePath + L" - отдаём оригинал игры");
        }

        // 2. Автопатч: берём оригинал прямо из exe игры, врезаем перевод в
        //    памяти. Никаких заранее подготовленных файлов - значит и
        //    обновление игры ничего не ломает.
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (g_notPatchable.count(path)) {
                if (deferral) deferral->Complete();
                return S_OK;
            }
            auto cached = g_patchedCache.find(path);
            if (cached != g_patchedCache.end()) {
                RespondWith(keepArgs, deferral, cached->second, path);
                return S_OK;
            }
        }

        // Распаковка чанка (до нескольких мегабайт) и поиск в нём функции -
        // работа не для потока пользовательского интерфейса: пока она идёт,
        // игра не должна подвисать. Отложенный ответ это и позволяет.
        std::thread([keepArgs, deferral, path]() {
            std::string original;
            std::shared_ptr<std::string> patched;

            if (GameAssets::Read(path, original)) {
                auto result = std::make_shared<std::string>();
                if (Locale::PatchJs(original, *result)) {
                    patched = result;
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_cacheMutex);
                if (patched) {
                    g_patchedCache[path] = patched;
                } else {
                    // Обычный случай: большинство чанков к локализации
                    // отношения не имеют. Запоминаем, чтобы не пытаться снова.
                    g_notPatchable.insert(path);
                }
            }

            if (patched) LogServed(path, patched->size(), L"собран из exe на лету");
            RespondWith(keepArgs, deferral, patched, path);
        }).detach();

        return S_OK;
    }

} // namespace

namespace {

    // Общая часть: подписаться на WebResourceRequested. Делается дважды -
    // до и после того, как свой обработчик зарегистрирует сама игра
    // (см. комментарий у AssetPatch::InstallLate).
    bool AddHandler(ICoreWebView2* webview)
    {
        EventRegistrationToken token;
        return SUCCEEDED(webview->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT
                {
                    return OnWebResourceRequested(args);
                }).Get(),
            &token));
    }

} // namespace

namespace AssetPatch {

void Install(ICoreWebView2* webview)
{
    if (!webview) return;

    // Основной режим - автопатч: перевод берётся из locale_ru.json, а
    // оригиналы чанков - прямо из exe игры (gameassets.cpp). Заранее
    // подготовленные файлы не нужны, поэтому обновление игры (а с ним и
    // новые имена чанков с хешами) ничего не ломает.
    const size_t localeKeys = Locale::Reload();

    // Ручной режим оставлен как запасной путь и способ подложить что угодно:
    // файл из этой папки перебивает автопатч.
    g_patchRoot = GetDllFolder() + L"\\locale_patch";
    std::vector<std::wstring> files;
    if (DirectoryExists(g_patchRoot)) {
        CollectPatchFiles(g_patchRoot, L"", files);
    } else {
        g_patchRoot.clear();
    }

    if (localeKeys == 0 && files.empty()) {
        WriteLog(L"[assets] Ни locale_ru.json, ни папки locale_patch рядом с DLL нет - "
                 L"подмена файлов игры выключена (остаётся словарь по DOM)");
        return;
    }

    // Разбор таблицы ассетов - это линейный поиск по сотням мегабайт образа,
    // поэтому делаем его заранее и в фоне, чтобы он не пришёлся на первый же
    // запрос страницы.
    if (localeKeys > 0) {
        std::thread([]() { GameAssets::Load(); }).detach();
    }

    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(webview->QueryInterface(IID_PPV_ARGS(&webview2))) || !webview2
        || FAILED(webview2->get_Environment(&g_environment)) || !g_environment) {
        WriteLog(L"[assets] Не удалось получить ICoreWebView2Environment - "
                 L"подмена файлов игры невозможна на этом рантайме");
        return;
    }

    // Фильтры ставим не на "*", а по делу: на js-файлы (их патчит автопатч)
    // и на конкретные файлы из locale_patch. Иначе обработчик дёргался бы на
    // каждый шрифт и каждую картинку игры.
    //
    // Про источники запросов: обычного AddWebResourceRequestedFilter хватает
    // для запросов самой страницы, но каталог локали нужен ещё и воркерам
    // (в игре их несколько, и часть текста собирается именно там). Поэтому
    // если рантайм умеет ICoreWebView2_22, просим все виды источников сразу.
    ComPtr<ICoreWebView2_22> webview22;
    bool withSourceKinds = SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview22))) && webview22;

    std::vector<std::wstring> filters;
    if (localeKeys > 0) {
        filters.push_back(L"*.js");
        filters.push_back(L"*.mjs");
    }
    for (const auto& rel : files) {
        filters.push_back(L"*/" + rel);
    }

    for (const auto& filter : filters) {
        HRESULT hr;
        if (withSourceKinds) {
            hr = webview22->AddWebResourceRequestedFilterWithRequestSourceKinds(
                filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
                COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
        } else {
            hr = webview->AddWebResourceRequestedFilter(
                filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        }
        if (FAILED(hr)) {
            WriteLog(L"[assets] Не удалось поставить фильтр на " + filter);
        }
    }

    if (!AddHandler(webview)) {
        WriteLog(L"[assets] add_WebResourceRequested не удался - подмена файлов не работает");
        return;
    }

    g_active = true;
    WriteLog(L"[assets] Подмена файлов игры включена: строк перевода "
             + std::to_wstring(localeKeys) + L", готовых файлов в locale_patch "
             + std::to_wstring(files.size())
             + (withSourceKinds ? L" (включая запросы воркеров)"
                                : L" (рантайм старый: только запросы страницы)"));

    // Имена js-чанков игры содержат хеш содержимого, поэтому после
    // обновления игры патч перестаёт подходить: имена в locale_patch уже
    // не те, что запрашивает страница. Само по себе это безопасно (мы
    // просто ничего не подменяем, игра идёт на английском), но молча -
    // и выглядит как "перевод сломался". Поэтому по окончании загрузки
    // страницы прямо говорим, что патч устарел.
    EventRegistrationToken navToken;
    webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT
            {
                static std::atomic<bool> reported{false};
                if (reported.exchange(true)) return S_OK;

                if (g_servedCount == 0) {
                    // Раньше это чаще всего означало "патч устарел после
                    // обновления игры". С автопатчем такой причины больше нет,
                    // остаются две: не нашлась таблица ассетов в exe (формат
                    // изменился) или не нашлась функция поиска перевода.
                    WriteLog(L"[assets] ВНИМАНИЕ: страница загрузилась, но ни один файл "
                             L"подменить не удалось. Смотрите выше строки [gameassets] и "
                             L"[locale]: либо в exe не опознана таблица ассетов, либо в чанках "
                             L"не нашлась функция поиска перевода (игра могла её изменить). "
                             L"Сама игра при этом работает как обычно, просто без перевода.");
                } else {
                    WriteLog(L"[assets] Файлов подменено за загрузку страницы: "
                             + std::to_wstring(g_servedCount.load()));
                }
                return S_OK;
            }).Get(),
        &navToken);
}

void ReloadLocale()
{
    const size_t keys = Locale::Reload();
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_patchedCache.clear();
        g_notPatchable.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_alreadyLogged.clear();
    }
    WriteLog(L"[assets] Каталог перевода перечитан (строк: " + std::to_wstring(keys)
             + L"), чанки будут собраны заново");
}

void InstallLate(ICoreWebView2Controller* controller)
{
    if (!controller || !g_active) return;   // подмена не включалась

    ComPtr<ICoreWebView2> webview;
    if (FAILED(controller->get_CoreWebView2(&webview)) || !webview) return;

    if (AddHandler(webview.Get())) {
        WriteLog(L"[assets] Обработчик перерегистрирован после игры - теперь "
                 L"наш ответ имеет приоритет над файлами из exe");
    } else {
        WriteLog(L"[assets] Повторная регистрация обработчика не удалась - "
                 L"подмену, скорее всего, перебьёт сама игра");
    }
}

} // namespace AssetPatch
