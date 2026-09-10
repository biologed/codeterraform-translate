#include "webview.h"
#include "hooks.h"
#include "translate.h"
#include "assets.h"

#include <wrl.h>
#include <fstream>
#include <atomic>
#include <string>
#include <cstring>
#include <mutex>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

using namespace Microsoft::WRL;
using json = nlohmann::json;

namespace {
    ComPtr<ICoreWebView2> g_webview;
    ComPtr<ICoreWebView2Controller> g_controller;
    std::wstring g_scriptId;                 // id скрипта, зарегистрированного через AddScriptToExecuteOnDocumentCreated
    std::atomic<bool> g_hotkeyThreadStarted{false};
     
    // ВАЖНО: WebView2 - это STA COM-объект, привязанный к тому потоку, на
    // котором был создан контроллер (обычно главный/UI-поток игры с
    // сообщенческим циклом). Все методы вроде ExecuteScript ОБЯЗАНЫ
    // вызываться с ЭТОГО ЖЕ потока - вызов с чужого потока (как наш
    // HotkeyThread, отдельный поток, который мы сами создаём в DllMain)
    // либо тихо ничего не делает, либо падает без всякой ошибки в лог,
    // потому что сам ExecuteScript() может вернуть неудачный HRESULT ещё
    // ДО того, как дойдёт до колбэка - а колбэк мы и логируем, сам вызов
    // ExecuteScript() - нет. Именно это, а не что-то в самом JS, было
    // причиной того, что F10 (и, скорее всего, "живой" F11-reload) молчали
    // без единой строчки в логе.
    //
    // Чтобы вызывать WebView2 API с ПРАВИЛЬНОГО потока из хоткей-потока,
    // подменяем (subclass) оконную процедуру окна самой игры (получаем его
    // через ICoreWebView2Controller::get_ParentWindow) и передаём туда
    // работу через обычное PostMessage с зарегистрированным сообщением -
    // оконная процедура физически исполняется на потоке-владельце окна,
    // так что внутри неё вызывать WebView2 API уже безопасно.
    HWND g_hostWindow = nullptr;
    WNDPROC g_originalWndProc = nullptr;
    std::mutex g_pendingMutex;
    std::vector<std::function<void()>> g_pendingWork;

    // Обратная к WideToUtf8 из hooks.h: текст из JSON приходит в UTF-8,
    // а WriteLog принимает "широкие" строки.
    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
        return w;
    }

    UINT DispatchMessageId()
    {
        static UINT id = RegisterWindowMessageW(L"Translator_Dll_Dispatch_9f3a1c");
        return id;
    }

    LRESULT CALLBACK SubclassWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == DispatchMessageId()) {
            std::vector<std::function<void()>> work;
            {
                std::lock_guard<std::mutex> lock(g_pendingMutex);
                work.swap(g_pendingWork);
            }
            for (auto& fn : work) fn();
            return 0;
        }
        return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);
    }

}

namespace WebViewHooks {

// Ставит переданную функцию в очередь и будит окно игры сообщением -
// функция реально выполнится позже, изнутри SubclassWndProc, то есть
// на правильном потоке. Если по какой-то причине окно ещё не найдено -
// откатываемся на прямой вызов (лучше так, чем вообще ничего).
void RunOnUiThread(std::function<void()> fn)
{
    if (!g_hostWindow) {
        WriteLog(L"[webview] RunOnUiThread: окно игры ещё не найдено, вызываем напрямую (может не сработать)");
        fn();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingWork.push_back(std::move(fn));
    }
    PostMessageW(g_hostWindow, DispatchMessageId(), 0, 0);
}

} // namespace WebViewHooks

// Записывает в dump.txt всё, что страница прислала через
// window.chrome.webview.postMessage(...).
static void OnWebMessageReceived(ICoreWebView2* /*sender*/, ICoreWebView2WebMessageReceivedEventArgs* args)
{
    LPWSTR message = nullptr;
    if (SUCCEEDED(args->TryGetWebMessageAsString(&message)) && message) {
        // std::ofstream (не wofstream!) + ручная конвертация в UTF-8 -
        // иначе не-ASCII текст (например русский из самой игры) обрежется
        // на первом же таком символе, см. подробности в hooks.cpp/WriteLog.
        std::ofstream dump(GetDllFolder() + L"\\dump.txt", std::ios::app | std::ios::binary);
        if (dump.is_open()) {
            dump << WideToUtf8(message) << "\n----\n";
        }
        CoTaskMemFree(message);
    }
}

// (Пере)регистрирует скрипт перевода, который WebView2 будет сам
// автоматически внедрять в начале КАЖДОГО документа (то есть при каждой
// навигации), ещё до того как выполнится любой скрипт самой страницы.
static void RegisterBootstrapScript()
{
    if (!g_webview) return;

    if (!g_scriptId.empty()) {
        g_webview->RemoveScriptToExecuteOnDocumentCreated(g_scriptId.c_str());
        g_scriptId.clear();
    }

    g_webview->AddScriptToExecuteOnDocumentCreated(
        Translate::BuildBootstrapScript().c_str(),
        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [](HRESULT error, LPCWSTR id) -> HRESULT
            {
                if (SUCCEEDED(error)) {
                    if (id) g_scriptId = id;
                    WriteLog(L"[webview] Скрипт перевода зарегистрирован");
                } else {
                    WriteLog(L"[webview] Не удалось зарегистрировать скрипт перевода");
                }
                return S_OK;
            }).Get());
}

// Поток с горячими клавишами для ручной отладки без пересборки DLL:
//   F6  - перечитать locale_ru.json и перезагрузить страницу (перевод через
//         локаль игры применяется без перезапуска игры)
//   F7  - записать в dump_focus.txt, что за элемент под курсором/фокусом и
//         защищён ли он от перевода (подбор settings.skipSelectors)
//   F9  - сохранить document.body.innerText в dump.txt + накопить заготовку
//         словаря по текущему экрану в dump_dom.json
//   F10 - сохранить document.body.outerHTML (РАЗМЕТКУ, не только текст) в
//         dump_html.txt - основной способ понять, из каких тегов реально
//         состоит непереводящаяся фраза (DevTools в этой игре заблокированы
//         чем-то, что мы не смогли найти в реестре - убраны за ненадобностью)
//   F11 - перечитать translations.json и применить перевод заново
static DWORD WINAPI HotkeyThread(LPVOID)
{
    bool f6Down = false, f7Down = false, f9Down = false, f10Down = false, f11Down = false;
    while (true) {
        bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;

        // ВАЖНО: не вызываем WebViewHooks::* напрямую - этот поток НЕ
        // владеет WebView2 COM-объектами (см. комментарий у RunOnUiThread
        // выше), поэтому вызов маршалится на правильный поток через окно игры.
        if (f6 && !f6Down) WebViewHooks::RunOnUiThread([]() { WebViewHooks::ReloadLocale(); });
        if (f7 && !f7Down) WebViewHooks::RunOnUiThread([]() { WebViewHooks::DumpFocusInfo(); });
        if (f9 && !f9Down) WebViewHooks::RunOnUiThread([]() { WebViewHooks::DumpBodyText(); });
        if (f10 && !f10Down) WebViewHooks::RunOnUiThread([]() { WebViewHooks::DumpBodyHtml(L""); });
        if (f11 && !f11Down) WebViewHooks::RunOnUiThread([]() { WebViewHooks::ReloadTranslations(); });

        f6Down = f6;
        f7Down = f7;
        f9Down = f9;
        f10Down = f10;
        f11Down = f11;
        Sleep(50);
    }
    return 0; // недостижимо, но убирает предупреждение компилятора
}

// ExecuteScript всегда отдаёт результат как JSON-литерал (строка приезжает
// в кавычках и экранированная) - json::parse снимает кавычки и разэкранирует
// обратно в настоящий текст. Возвращает false, если результат не строка.
static bool ResultAsString(LPCWSTR resultObjectAsJson, const wchar_t* who, std::string& out)
{
    try {
        json j = json::parse(WideToUtf8(resultObjectAsJson ? resultObjectAsJson : L"null"));
        if (!j.is_string()) {
            WriteLog(std::wstring(L"[webview] ") + who + L": неожиданный тип результата (не строка)");
            return false;
        }
        out = j.get<std::string>();
        return true;
    } catch (const std::exception& e) {
        WriteLog(std::wstring(L"[webview] ") + who + L": не удалось разобрать результат ExecuteScript: "
            + std::wstring(e.what(), e.what() + strlen(e.what())));
        return false;
    }
}

// Копит заготовку словаря между нажатиями F9: файл читается, новые строки
// дописываются, уже имеющиеся (в том числе уже переведённые вручную) не
// трогаются. Иначе пришлось бы после каждого экрана сливать дампы руками.
static void MergeDictSkeleton(const std::wstring& fileName, const json& strings)
{
    std::wstring path = GetDllFolder() + L"\\" + fileName;

    json skeleton = json::object();
    {
        std::ifstream in(path, std::ios::binary);
        if (in.is_open()) {
            try {
                json parsed = json::parse(in, nullptr, false, true);
                if (parsed.is_object()) skeleton = std::move(parsed);
            } catch (const std::exception&) {
                WriteLog(L"[webview] " + fileName + L" повреждён - будет перезаписан заново");
            }
        }
    }

    size_t added = 0;
    for (const auto& s : strings) {
        if (!s.is_string()) continue;
        const std::string key = s.get<std::string>();
        if (key.empty() || skeleton.contains(key)) continue;
        // Значение = оригинал: файл можно сразу скормить переводчику, и
        // до правки он ничего не испортит (строка "переведётся" сама в себя).
        skeleton[key] = key;
        added++;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        WriteLog(L"[webview] Не удалось записать " + fileName);
        return;
    }
    out << skeleton.dump(2);
    WriteLog(L"[webview] " + fileName + L": новых строк " + std::to_wstring(added)
             + L", всего " + std::to_wstring(skeleton.size()));
}

// Через несколько секунд после загрузки страницы спрашиваем у неё, чем всё
// закончилось с локалью. Почему с задержкой и почему вообще отдельно:
// подмену файла видно сразу (её пишет assets.cpp), а вот применилась ли
// локаль ВНУТРИ игры - нет, потому что игра выбирает язык асинхронно, уже
// после загрузки документа. Без этой строчки в логе "перевод не появился"
// невозможно отличить от "патч не доехал".
static void ScheduleStartupDiagnostics()
{
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(5000);
        WebViewHooks::RunOnUiThread([]() {
            WebViewHooks::ExecuteJs(
                L"JSON.stringify({"
                L"  documentLang: document.documentElement ? document.documentElement.lang : null,"
                // Сколько наших патченных модулей реально исполнилось на странице.
                // null здесь при успешной подмене файла означает, что страница
                // выполнила НЕ тот файл, который мы отдали (кэш, дубликат чанка).
                L"  patchLoaded: window.__TR_PATCH_LOADED || null,"
                L"  localeApplied: window.__TR_LOCALE_APPLIED || null,"
                L"  localeError: window.__TR_LOCALE_ERROR || null,"
                // Размеры реально загруженных js: подменённый чанк весит на
                // мегабайты больше оригинала, так что по размеру сразу видно,
                // чей файл выполнила страница - наш или игры.
                L"  loadedJs: (performance.getEntriesByType('resource') || [])"
                L"    .filter(function(e) { return /i18n|worker/i.test(e.name); })"
                L"    .map(function(e) { return e.name.split('/').pop() + ':' +"
                L"        (e.encodedBodySize || e.transferSize || 0); }),"
                L"  screen: document.body ? document.body.innerText.slice(0, 60) : null"
                L"})");
        });
        return 0;
    }, nullptr, 0, nullptr);
}

namespace WebViewHooks {

void ExecuteJs(const std::wstring& js)
{
    if (!g_webview) {
        WriteLog(L"[webview] ExecuteJs вызван до готовности webview - игнорируем");
        return;
    }

    g_webview->ExecuteScript(js.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT error, LPCWSTR resultObjectAsJson) -> HRESULT
            {
                if (FAILED(error)) {
                    WriteLog(L"[webview] ExecuteScript вернул ошибку: " + std::to_wstring(error));
                    return S_OK;
                }
                // Логируем результат ВСЕГДA (не только ошибки) - именно так
                // видно диагностику из BuildBootstrapScript() (url, сколько
                // строк реально заменил F11 на уже открытой странице и т.д.),
                // без чего пришлось бы гадать, почему живой reload не всегда
                // виден на экране.
                if (resultObjectAsJson && *resultObjectAsJson) {
                    WriteLog(L"[webview] ExecuteScript результат: " + std::wstring(resultObjectAsJson));
                }
                return S_OK;
            }).Get());
}

void DumpBodyText()
{
    if (!g_webview) {
        WriteLog(L"[webview] DumpBodyText вызван до готовности webview - игнорируем");
        return;
    }

    // Раньше дамп делался через window.chrome.webview.postMessage() - но
    // в этой (Tauri) игре в dump.txt прилетали ТОЛЬКО внутренние IPC-сообщения
    // самого Tauri (вида {"cmd":"plugin:fs|exists",...}), а не наш текст.
    // Похоже, Tauri сам подменяет/оборачивает window.chrome.webview.postMessage
    // под свой internal IPC, и наш вызов до настоящего нативного postMessage
    // просто не доходит (или доходит, но теряется/не совпадает с тем, что
    // ожидает обёртка). Обходим это полностью: ExecuteScript сам возвращает
    // результат вычисления выражения через колбэк - берём document.body.innerText
    // оттуда напрямую, никакого postMessage вообще не нужно.
    // Забираем за один вызов и сырой текст экрана (dump.txt, читать глазами),
    // и список строк-кандидатов от window.__translator.collect() - он уже
    // умеет пропускать зоны пользовательского ввода и то, что в словаре есть.
    // Собирать этот список тут же, в C++, нельзя: логика "что защищено"
    // живёт в скрипте перевода (translate.cpp), и дублировать её - значит
    // рано или поздно разойтись с ней.
    g_webview->ExecuteScript(
        L"JSON.stringify({"
        L"  text: document.body ? document.body.innerText : '',"
        L"  strings: window.__translator ? window.__translator.collect() : null"
        L"})",
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT error, LPCWSTR resultObjectAsJson) -> HRESULT
            {
                if (FAILED(error)) {
                    WriteLog(L"[webview] DumpBodyText: ExecuteScript вернул ошибку: " + std::to_wstring(error));
                    return S_OK;
                }

                std::string payload;
                if (!ResultAsString(resultObjectAsJson, L"DumpBodyText", payload)) return S_OK;

                json data;
                try {
                    data = json::parse(payload);
                } catch (const std::exception& e) {
                    WriteLog(L"[webview] DumpBodyText: не удалось разобрать дамп: "
                        + std::wstring(e.what(), e.what() + strlen(e.what())));
                    return S_OK;
                }

                std::ofstream dump(GetDllFolder() + L"\\dump.txt", std::ios::app | std::ios::binary);
                if (dump.is_open()) {
                    dump << data.value("text", std::string()) << "\n----\n";
                }

                if (data["strings"].is_array()) {
                    MergeDictSkeleton(L"dump_dom.json", data["strings"]);
                } else {
                    WriteLog(L"[webview] DumpBodyText: скрипт перевода на странице ещё не запускался - "
                             L"собран только текст, без заготовки словаря");
                }
                return S_OK;
            }).Get());
}

void DumpFocusInfo()
{
    if (!g_webview) {
        WriteLog(L"[webview] DumpFocusInfo вызван до готовности webview - игнорируем");
        return;
    }

    g_webview->ExecuteScript(
        L"JSON.stringify(window.__translator ? window.__translator.describe()"
        L" : {error: 'скрипт перевода на этой странице ещё не запускался'}, null, 2)",
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT error, LPCWSTR resultObjectAsJson) -> HRESULT
            {
                if (FAILED(error)) {
                    WriteLog(L"[webview] DumpFocusInfo: ExecuteScript вернул ошибку: " + std::to_wstring(error));
                    return S_OK;
                }

                std::string text;
                if (!ResultAsString(resultObjectAsJson, L"DumpFocusInfo", text)) return S_OK;

                std::ofstream dump(GetDllFolder() + L"\\dump_focus.txt", std::ios::app | std::ios::binary);
                if (dump.is_open()) {
                    dump << text << "\n----\n";
                }

                // Короткую выжимку дублируем в лог: обычно достаточно её,
                // не открывая сам файл.
                try {
                    json j = json::parse(text);
                    for (const char* which : {"active", "hovered"}) {
                        if (!j.contains(which) || !j[which].is_object()) continue;
                        const json& el = j[which];
                        WriteLog(L"[webview] F7 " + std::wstring(which, which + strlen(which)) + L": "
                            + Utf8ToWide(el.value("path", std::string()))
                            + L" | защищён: " + (el.value("protectedFromTranslation", false) ? L"да" : L"НЕТ")
                            + L" | селектор: " + Utf8ToWide(el["matchedSkipSelector"].is_string()
                                ? el["matchedSkipSelector"].get<std::string>()
                                : std::string("-"))
                            + L" | можно добавить: " + Utf8ToWide(el["suggestedSkipSelector"].is_string()
                                ? el["suggestedSkipSelector"].get<std::string>()
                                : std::string("-")));
                    }
                } catch (const std::exception&) {
                    // Не критично: сам dump_focus.txt уже записан.
                }
                return S_OK;
            }).Get());
}

void ReloadLocale()
{
    AssetPatch::ReloadLocale();
    if (g_webview) {
        // Перезагрузка страницы заставляет игру заново запросить js-чанки -
        // и они соберутся уже с новым каталогом.
        g_webview->Reload();
        WriteLog(L"[webview] Страница перезагружена с новым каталогом перевода");
    }
}

void ReloadTranslations()
{
    Translate::Reload();
    RegisterBootstrapScript();       // чтобы будущие навигации тоже использовали новый словарь
    ExecuteJs(Translate::BuildBootstrapScript()); // и сразу применить на текущей странице
}

// Основной способ посмотреть DOM-структуру непереводящейся фразы (DevTools
// в этой игре чем-то заблокированы - причину в реестре найти не удалось,
// поэтому саму функцию открытия DevTools убрали за ненадобностью). Вместо
// визуального инспектора просто выгружаем РАЗМЕТКУ (а не только видимый
// текст) нужного элемента через самый обычный ExecuteScript - никаких
// особых прав для этого не требуется, это такой же JS, как и весь
// остальной перевод.
//
// document.body.outerHTML целиком может быть огромным, поэтому по
// умолчанию берём querySelector по переданному CSS-селектору (например,
// на элемент с конкретным непереведённым текстом) - если селектор не
// передан или не найден, откатываемся на document.body.outerHTML целиком.
void DumpBodyHtml(const std::wstring& cssSelector)
{
    if (!g_webview) {
        WriteLog(L"[webview] DumpBodyHtml вызван до готовности webview - игнорируем");
        return;
    }

    std::wstring js =
        L"(function(sel){"
        L"  var el = sel ? document.querySelector(sel) : null;"
        L"  return (el || document.body).outerHTML;"
        L"})(" + (cssSelector.empty() ? L"null" : (L"'" + cssSelector + L"'")) + L");";

    g_webview->ExecuteScript(js.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT error, LPCWSTR resultObjectAsJson) -> HRESULT
            {
                if (FAILED(error)) {
                    WriteLog(L"[webview] DumpBodyHtml: ExecuteScript вернул ошибку: " + std::to_wstring(error));
                    return S_OK;
                }

                std::string text;
                try {
                    json j = json::parse(WideToUtf8(resultObjectAsJson ? resultObjectAsJson : L"null"));
                    if (j.is_string()) {
                        text = j.get<std::string>();
                    } else {
                        WriteLog(L"[webview] DumpBodyHtml: неожиданный тип результата (не строка)");
                    }
                } catch (const std::exception& e) {
                    WriteLog(L"[webview] DumpBodyHtml: не удалось разобрать результат ExecuteScript: "
                        + std::wstring(e.what(), e.what() + strlen(e.what())));
                }

                std::ofstream dump(GetDllFolder() + L"\\dump_html.txt", std::ios::app | std::ios::binary);
                if (dump.is_open()) {
                    dump << text << "\n----\n";
                }
                WriteLog(L"[webview] dump_html.txt обновлён (" + std::to_wstring(text.size()) + L" байт)");
                return S_OK;
            }).Get());
}

void OnControllerCreated(ICoreWebView2Controller* controller)
{
    g_controller = controller;

    ComPtr<ICoreWebView2> webview;
    if (FAILED(controller->get_CoreWebView2(&webview)) || !webview) {
        WriteLog(L"[webview] get_CoreWebView2 не удался");
        return;
    }
    g_webview = webview;

    // Подключаемся (subclass) к окну игры, чтобы иметь возможность
    // безопасно вызывать WebView2 API с ПРАВИЛЬНОГО потока по хоткеям -
    // см. большой комментарий у RunOnUiThread/g_hostWindow выше.
    HWND hwnd = nullptr;
    if (!g_hostWindow && SUCCEEDED(controller->get_ParentWindow(&hwnd)) && hwnd) {
        g_hostWindow = hwnd;
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SubclassWndProc)));
        if (g_originalWndProc) {
            WriteLog(L"[webview] Подключились к окну игры (subclass) для маршалинга F9/F10/F11 на правильный поток");
        } else {
            WriteLog(L"[webview] Не удалось подменить оконную процедуру - F9/F10/F11 могут не сработать");
            g_hostWindow = nullptr;
        }
    }

    // ВАЖЕН ПОРЯДОК: подмену файлов ставим ПЕРВОЙ, до всего остального.
    // Игра ещё не начала навигацию (контроллер только что создан), значит
    // запрос за js-чанком с локалью ещё впереди - и мы успеем его перехватить.
    AssetPatch::Install(g_webview.Get());

    RegisterBootstrapScript();

    EventRegistrationToken token;
    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
            {
                OnWebMessageReceived(sender, args);
                return S_OK;
            }).Get(),
        &token);

    if (!g_hotkeyThreadStarted.exchange(true)) {
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
        ScheduleStartupDiagnostics();
    }

    WriteLog(L"[webview] ICoreWebView2 готов, перевод активирован "
             L"(F9 = дамп текста, F10 = дамп HTML, F11 = перезагрузить словарь)");
}

} // namespace WebViewHooks
