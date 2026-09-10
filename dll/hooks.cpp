#include "hooks.h"
#include "assets.h"
#include "webview.h"

#include <MinHook.h>
#include <wrl.h>
#include <WebView2.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <objbase.h>

#include <unordered_set>
#include <mutex>
#include <atomic>
#include <fstream>
#include <string>
#include <cstring>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

using namespace Microsoft::WRL;

// =====================================================================
//  Логирование и путь к DLL
// =====================================================================

std::wstring GetDllFolder()
{
    static std::wstring cached;
    if (!cached.empty()) return cached;

    wchar_t path[MAX_PATH] = {};
    HMODULE hModule = nullptr;

    // GetModuleHandleExW по адресу нашей же функции - надёжный способ
    // получить хендл СВОЕЙ dll, даже если её переименуют (например Xenos
    // любит копировать dll под случайным именем).
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&GetDllFolder),
        &hModule);

    GetModuleFileNameW(hModule, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    cached = path;
    return cached;
}

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), s.data(), len, nullptr, nullptr);
    return s;
}

void WriteLog(const std::wstring& message)
{
    // ВАЖНО: пишем как std::ofstream (не wofstream!) и сами конвертируем
    // в UTF-8. std::wofstream без явного imbue() использует классическую
    // "C"-локаль, которая не умеет кириллицу и других не-ASCII символы -
    // запись обрывается на первом же таком символе, а весь текст после
    // него молча теряется. Ручная конвертация в UTF-8 + бинарный режим
    // этого недостатка лишены.
    std::ofstream log(GetDllFolder() + L"\\translator_log.txt", std::ios::app | std::ios::binary);
    if (!log.is_open()) return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    sprintf_s(stamp, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    log << stamp << WideToUtf8(message) << "\n";
}

// =====================================================================
//  Оригинальные ("настоящие") функции - сюда MinHook положит трамплины
// =====================================================================

using CreateCoreWebView2EnvironmentWithOptions_t = HRESULT(STDMETHODCALLTYPE*)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler);

// Недокументированная (но подтверждённая официальной документацией
// Microsoft, см. webview2-idl reference) функция CreateWebViewEnvironmentWithOptionsInternal -
// экспорт из EmbeddedBrowserWebView.dll (реальный рантайм WebView2).
// Именно её вызывает статически слинкованный загрузчик, если приложение
// не использует отдельный WebView2Loader.dll (как в нашем случае).
// Сигнатура отличается от публичной: нет browserExecutableFolder, зато
// есть checkRunningInstance и runtimeType. environmentOptions передаётся
// как IUnknown* (а не ICoreWebView2EnvironmentOptions*) - осознанно слабо
// типизировано в этой внутренней функции.
using CreateWebViewEnvironmentWithOptionsInternal_t = HRESULT(STDMETHODCALLTYPE*)(
    bool checkRunningInstance,
    int runtimeType,
    PCWSTR userDataFolder,
    IUnknown* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* webViewEnvironmentCreatedHandler);

using CreateCoreWebView2Controller_t = HRESULT(STDMETHODCALLTYPE*)(
    ICoreWebView2Environment* self,
    HWND parentWindow,
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler);

// Два более новых способа создать controller (см. ICoreWebView2Environment10
// в WebView2.h) - для этой игры оказался нужен один из НИХ, а не базовый
// CreateCoreWebView2Controller (см. большой комментарий у HookEnvironment).
using CreateCoreWebView2ControllerWithOptions_t = HRESULT(STDMETHODCALLTYPE*)(
    ICoreWebView2Environment10* self,
    HWND parentWindow,
    ICoreWebView2ControllerOptions* options,
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler);

using CreateCoreWebView2CompositionControllerWithOptions_t = HRESULT(STDMETHODCALLTYPE*)(
    ICoreWebView2Environment10* self,
    HWND parentWindow,
    ICoreWebView2ControllerOptions* options,
    ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler* handler);

static CreateCoreWebView2EnvironmentWithOptions_t True_CreateCoreWebView2EnvironmentWithOptions = nullptr;
static CreateWebViewEnvironmentWithOptionsInternal_t True_CreateWebViewEnvironmentWithOptionsInternal = nullptr;
static CreateCoreWebView2Controller_t True_CreateCoreWebView2Controller = nullptr;
static CreateCoreWebView2ControllerWithOptions_t True_CreateCoreWebView2ControllerWithOptions = nullptr;
static CreateCoreWebView2CompositionControllerWithOptions_t True_CreateCoreWebView2CompositionControllerWithOptions = nullptr;

// Чтобы не поставить хук дважды на одну и ту же таблицу виртуальных функций,
// если приложение вдруг создаст несколько ICoreWebView2Environment.
static std::mutex g_hookedVtablesMutex;
static std::unordered_set<void*> g_hookedVtables;

// =====================================================================
//  Хук №2: ICoreWebView2Environment::CreateCoreWebView2Controller
//  Это COM-метод конкретного объекта, поэтому хукается не через
//  GetProcAddress, а через подмену указателя в его таблице виртуальных
//  функций (vtable). Индекс метода: 0=QueryInterface, 1=AddRef,
//  2=Release, 3=CreateCoreWebView2Controller (порядок объявления в
//  WebView2.h, проверено по официальным биндингам).
// =====================================================================

static HRESULT STDMETHODCALLTYPE Hooked_CreateCoreWebView2Controller(
    ICoreWebView2Environment* self,
    HWND parentWindow,
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler)
{
    WriteLog(L"[hooks] CreateCoreWebView2Controller() вызван игрой");

    // ComPtr сам вызовет AddRef при создании (см. конструктор от сырого
    // указателя) и Release, когда лямбда будет уничтожена. Это важно:
    // без этого originalHandler мог бы быть освобождён раньше, чем мы
    // его вызовем.
    ComPtr<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> originalHandler(handler);

    auto wrapped = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [originalHandler](HRESULT errorCode, ICoreWebView2Controller* controller) -> HRESULT
        {
            if (SUCCEEDED(errorCode) && controller) {
                WriteLog(L"[hooks] ICoreWebView2Controller получен, инициализируем перевод");
                WebViewHooks::OnControllerCreated(controller);
            } else {
                WriteLog(L"[hooks] Ошибка создания controller, код: " + std::to_wstring(errorCode));
            }

            // Колбэк игры вызываем ДО перерегистрации: внутри него Tauri
            // ставит собственный обработчик WebResourceRequested, а выигрывает
            // тот, кто зарегистрировался последним (см. AssetPatch::InstallLate).
            HRESULT result = originalHandler ? originalHandler->Invoke(errorCode, controller) : S_OK;
            if (SUCCEEDED(errorCode) && controller) {
                AssetPatch::InstallLate(controller);
            }
            return result;
        });

    return True_CreateCoreWebView2Controller(self, parentWindow, wrapped.Get());
}

// =====================================================================
//  Хук №2б и №2в: два более новых способа создать controller.
//
//  Диагностика (хук QueryInterface, см. ниже) на этой конкретной игре
//  показала, что после получения environment она делает QueryInterface на
//  GUID {EE0EB9DF-6F12-46CE-B53F-3F47B9C928E0} - это ICoreWebView2Environment10
//  (проверено по официальному WebView2.h). У него ТРИ своих метода (кроме
//  унаследованных): CreateCoreWebView2ControllerOptions,
//  CreateCoreWebView2ControllerWithOptions,
//  CreateCoreWebView2CompositionControllerWithOptions. Игра создаёт
//  controller одним из последних двух (не старым базовым методом, слот 3
//  которого мы уже хукали раньше - именно поэтому он не срабатывал).
//  Так как не известно точно, каким из двух - хукаем ОБА.
//
//  CreateCoreWebView2CompositionControllerWithOptions отдаёт не
//  ICoreWebView2Controller, а ICoreWebView2CompositionController (для
//  GPU-композитного рендера) - это ОТДЕЛЬНЫЙ интерфейс (не наследник
//  ICoreWebView2Controller), но реальный объект рантайма реализует ОБА
//  сразу, поэтому обычный QueryInterface с него получает нужный нам
//  ICoreWebView2Controller.
// =====================================================================

static HRESULT STDMETHODCALLTYPE Hooked_CreateCoreWebView2ControllerWithOptions(
    ICoreWebView2Environment10* self,
    HWND parentWindow,
    ICoreWebView2ControllerOptions* options,
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler)
{
    WriteLog(L"[hooks] CreateCoreWebView2ControllerWithOptions() вызван игрой");

    ComPtr<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> originalHandler(handler);

    auto wrapped = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [originalHandler](HRESULT errorCode, ICoreWebView2Controller* controller) -> HRESULT
        {
            if (SUCCEEDED(errorCode) && controller) {
                WriteLog(L"[hooks] ICoreWebView2Controller получен (через WithOptions), инициализируем перевод");
                WebViewHooks::OnControllerCreated(controller);
            } else {
                WriteLog(L"[hooks] Ошибка создания controller (WithOptions), код: " + std::to_wstring(errorCode));
            }

            // Колбэк игры вызываем ДО перерегистрации: внутри него Tauri
            // ставит собственный обработчик WebResourceRequested, а выигрывает
            // тот, кто зарегистрировался последним (см. AssetPatch::InstallLate).
            HRESULT result = originalHandler ? originalHandler->Invoke(errorCode, controller) : S_OK;
            if (SUCCEEDED(errorCode) && controller) {
                AssetPatch::InstallLate(controller);
            }
            return result;
        });

    return True_CreateCoreWebView2ControllerWithOptions(self, parentWindow, options, wrapped.Get());
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateCoreWebView2CompositionControllerWithOptions(
    ICoreWebView2Environment10* self,
    HWND parentWindow,
    ICoreWebView2ControllerOptions* options,
    ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler* handler)
{
    WriteLog(L"[hooks] CreateCoreWebView2CompositionControllerWithOptions() вызван игрой");

    ComPtr<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler> originalHandler(handler);

    auto wrapped = Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
        [originalHandler](HRESULT errorCode, ICoreWebView2CompositionController* compositionController) -> HRESULT
        {
            if (SUCCEEDED(errorCode) && compositionController) {
                WriteLog(L"[hooks] ICoreWebView2CompositionController получен, достаём ICoreWebView2Controller через QueryInterface");
                ComPtr<ICoreWebView2Controller> controller;
                if (SUCCEEDED(compositionController->QueryInterface(IID_PPV_ARGS(&controller))) && controller) {
                    WriteLog(L"[hooks] ICoreWebView2Controller получен (через composition), инициализируем перевод");
                    WebViewHooks::OnControllerCreated(controller.Get());
                } else {
                    WriteLog(L"[hooks] Не удалось получить ICoreWebView2Controller из composition controller");
                }
            } else {
                WriteLog(L"[hooks] Ошибка создания composition controller, код: " + std::to_wstring(errorCode));
            }

            HRESULT result = originalHandler ? originalHandler->Invoke(errorCode, compositionController) : S_OK;
            if (SUCCEEDED(errorCode) && compositionController) {
                ComPtr<ICoreWebView2Controller> late;
                if (SUCCEEDED(compositionController->QueryInterface(IID_PPV_ARGS(&late))) && late) {
                    AssetPatch::InstallLate(late.Get());
                }
            }
            return result;
        });

    return True_CreateCoreWebView2CompositionControllerWithOptions(self, parentWindow, options, wrapped.Get());
}

// =====================================================================
//  Диагностический "шпион" на IUnknown::QueryInterface этого же объекта
//  (слот 0 - есть у ЛЮБОГО COM-объекта, сигнатура всегда одна и та же,
//  поэтому его безопасно хукать "вслепую", не зная реальный тип объекта).
//
//  ЗАЧЕМ: если после "ICoreWebView2Environment получен" в логе никогда
//  не появляется "CreateCoreWebView2Controller() вызван игрой" - это
//  значит, что игра создаёт контроллер НЕ через этот (базовый, самый
//  старый) метод. У WebView2 давно есть более новые варианты - например
//  CreateCoreWebView2CompositionController (для GPU-композитного рендера,
//  через ICoreWebView2Environment3) или CreateCoreWebView2ControllerWithOptions
//  (ICoreWebView2Environment6+). Чтобы это выяснить, а не гадать - логируем
//  КАЖДЫЙ запрошенный интерфейс (GUID) через QueryInterface на самом
//  environment: там будет видно, за каким расширенным интерфейсом игра
//  обращается перед созданием контроллера, и это укажет точно, какой
//  метод хукать вместо/вместе с CreateCoreWebView2Controller.
// =====================================================================

using EnvQueryInterface_t = HRESULT(STDMETHODCALLTYPE*)(void* self, REFIID riid, void** ppvObject);
static EnvQueryInterface_t True_Environment_QueryInterface = nullptr;

static HRESULT STDMETHODCALLTYPE Hooked_Environment_QueryInterface(void* self, REFIID riid, void** ppvObject)
{
    LPOLESTR guidStr = nullptr;
    if (SUCCEEDED(StringFromIID(riid, &guidStr)) && guidStr) {
        WriteLog(L"[hooks] Environment::QueryInterface запросил интерфейс: " + std::wstring(guidStr));
        CoTaskMemFree(guidStr);
    }
    return True_Environment_QueryInterface(self, riid, ppvObject);
}

static void HookEnvironment(ICoreWebView2Environment* env)
{
    void** vtable = *reinterpret_cast<void***>(env);

    {
        std::lock_guard<std::mutex> lock(g_hookedVtablesMutex);
        if (g_hookedVtables.count(vtable)) {
            return; // уже хукнуто
        }
        g_hookedVtables.insert(vtable);
    }

    void* qiTarget = vtable[0]; // IUnknown::QueryInterface - диагностика, см. комментарий выше
    if (MH_CreateHook(qiTarget, &Hooked_Environment_QueryInterface,
                       reinterpret_cast<void**>(&True_Environment_QueryInterface)) == MH_OK &&
        MH_EnableHook(qiTarget) == MH_OK) {
        WriteLog(L"[hooks] Диагностический хук QueryInterface на environment установлен");
    } else {
        WriteLog(L"[hooks] Не удалось поставить диагностический хук QueryInterface");
    }

    void* target = vtable[3]; // CreateCoreWebView2Controller

    if (MH_CreateHook(target, &Hooked_CreateCoreWebView2Controller,
                       reinterpret_cast<void**>(&True_CreateCoreWebView2Controller)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        WriteLog(L"[hooks] Хук CreateCoreWebView2Controller (базовый, слот 3) установлен");
    } else {
        WriteLog(L"[hooks] MH_CreateHook/MH_EnableHook(CreateCoreWebView2Controller) не удался");
    }

    // Дополнительно: QueryInterface на ICoreWebView2Environment10 и хук
    // обоих его "WithOptions"-методов создания controller (слоты 21 и 22 -
    // см. комментарий у их определения выше). Именно один из них
    // фактически использует эта игра - базовый CreateCoreWebView2Controller
    // (слот 3 выше) для неё оказался мёртвым кодом, никогда не вызывается.
    ComPtr<ICoreWebView2Environment10> env10;
    if (SUCCEEDED(env->QueryInterface(IID_PPV_ARGS(&env10))) && env10) {
        void** vtable10 = *reinterpret_cast<void***>(env10.Get());

        void* withOptionsTarget = vtable10[21]; // CreateCoreWebView2ControllerWithOptions
        if (MH_CreateHook(withOptionsTarget, &Hooked_CreateCoreWebView2ControllerWithOptions,
                           reinterpret_cast<void**>(&True_CreateCoreWebView2ControllerWithOptions)) == MH_OK &&
            MH_EnableHook(withOptionsTarget) == MH_OK) {
            WriteLog(L"[hooks] Хук CreateCoreWebView2ControllerWithOptions (Environment10, слот 21) установлен");
        } else {
            WriteLog(L"[hooks] Не удалось поставить хук на CreateCoreWebView2ControllerWithOptions");
        }

        void* compositionTarget = vtable10[22]; // CreateCoreWebView2CompositionControllerWithOptions
        if (MH_CreateHook(compositionTarget, &Hooked_CreateCoreWebView2CompositionControllerWithOptions,
                           reinterpret_cast<void**>(&True_CreateCoreWebView2CompositionControllerWithOptions)) == MH_OK &&
            MH_EnableHook(compositionTarget) == MH_OK) {
            WriteLog(L"[hooks] Хук CreateCoreWebView2CompositionControllerWithOptions (Environment10, слот 22) установлен");
        } else {
            WriteLog(L"[hooks] Не удалось поставить хук на CreateCoreWebView2CompositionControllerWithOptions");
        }
    } else {
        WriteLog(L"[hooks] QueryInterface(ICoreWebView2Environment10) не удался - установленный рантайм старее ожидаемого");
    }
}

// =====================================================================
//  Поиск модуля, который реально экспортирует
//  CreateCoreWebView2EnvironmentWithOptions.
//
//  Изначально мы искали жёстко "WebView2Loader.dll" - это работает для
//  приложений, которые используют ДИНАМИЧЕСКИЙ вариант загрузчика (сам
//  файл WebView2Loader.dll лежит рядом с exe). Но многие Rust/Tauri-игры
//  (в частности эта) линкуют загрузчик СТАТИЧЕСКИ прямо в свой exe -
//  отдельного WebView2Loader.dll в файловой системе тогда просто нет, и
//  GetModuleHandleW никогда его не найдёт.
//
//  Реальная функция при этом всё равно должна где-то появиться в виде
//  экспорта - в самом рантайме Microsoft Edge WebView2 (реально это
//  EmbeddedBrowserWebView.dll из папки установки рантайма), который
//  грузится в процесс игры, когда та первый раз создаёт environment.
//  Поэтому вместо жёсткого имени файла перебираем ВСЕ загруженные в
//  процесс модули и ищем среди них тот, что реально экспортирует нужную
//  функцию - имя файла при этом неважно.
//
//  Проверяем ДВА возможных экспорта:
//   - CreateCoreWebView2EnvironmentWithOptions - публичная, документированная,
//     стабильная. Бывает в WebView2Loader.dll при динамической линковке.
//   - CreateWebViewEnvironmentWithOptionsInternal - внутренняя (но описанная
//     в справочнике webview2-idl), с другой сигнатурой. Именно её вызывает
//     статически слинкованный загрузчик, когда отдельного WebView2Loader.dll
//     в файловой системе нет (наш случай).
// =====================================================================

enum class WebView2EntryPointKind { None, Public, Internal };

static WebView2EntryPointKind FindWebView2ExportingModule(void** outProcAddress)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return WebView2EntryPointKind::None;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    WebView2EntryPointKind found = WebView2EntryPointKind::None;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (void* proc = reinterpret_cast<void*>(
                    GetProcAddress(entry.hModule, "CreateCoreWebView2EnvironmentWithOptions"))) {
                WriteLog(L"[hooks] Найден публичный экспорт CreateCoreWebView2EnvironmentWithOptions в модуле: "
                         + std::wstring(entry.szModule));
                *outProcAddress = proc;
                found = WebView2EntryPointKind::Public;
                break;
            }
            if (void* proc = reinterpret_cast<void*>(
                    GetProcAddress(entry.hModule, "CreateWebViewEnvironmentWithOptionsInternal"))) {
                WriteLog(L"[hooks] Найден внутренний экспорт CreateWebViewEnvironmentWithOptionsInternal в модуле: "
                         + std::wstring(entry.szModule));
                *outProcAddress = proc;
                found = WebView2EntryPointKind::Internal;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

// Если за отведённое время экспорт так и не нашёлся - выгружаем список
// вообще всех модулей процесса в лог, чтобы можно было вручную понять,
// куда смотреть дальше (например, если рантайм называется как-то иначе).
static void DumpLoadedModules()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    WriteLog(L"[hooks] Список всех загруженных модулей процесса:");
    if (Module32FirstW(snapshot, &entry)) {
        do {
            WriteLog(L"    " + std::wstring(entry.szExePath));
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

// =====================================================================
//  Хук №1: настоящая функция создания environment, где бы она физически
//  ни лежала и под каким бы именем ни была экспортирована (см. поиск
//  выше). Обе версии (публичная и internal) используют ОДИН и тот же
//  тип колбэка завершения - ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
//  поэтому логику его "обёртывания" выносим в общую функцию.
// =====================================================================

static ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
WrapEnvironmentHandler(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler)
{
    // ComPtr сам вызовет AddRef при создании (конструктор от сырого
    // указателя) и Release, когда лямбда будет уничтожена - без этого
    // originalHandler мог бы быть освобождён раньше, чем мы его вызовем.
    ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> originalHandler(handler);

    return Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [originalHandler](HRESULT errorCode, ICoreWebView2Environment* env) -> HRESULT
        {
            if (SUCCEEDED(errorCode) && env) {
                WriteLog(L"[hooks] ICoreWebView2Environment получен");
                HookEnvironment(env);
            } else {
                WriteLog(L"[hooks] Ошибка создания environment, код: " + std::to_wstring(errorCode));
            }

            if (originalHandler) {
                return originalHandler->Invoke(errorCode, env);
            }
            return S_OK;
        });
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateCoreWebView2EnvironmentWithOptions(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler)
{
    WriteLog(L"[hooks] CreateCoreWebView2EnvironmentWithOptions() вызван игрой");

    auto wrapped = WrapEnvironmentHandler(environmentCreatedHandler);

    return True_CreateCoreWebView2EnvironmentWithOptions(
        browserExecutableFolder, userDataFolder, environmentOptions, wrapped.Get());
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateWebViewEnvironmentWithOptionsInternal(
    bool checkRunningInstance,
    int runtimeType,
    PCWSTR userDataFolder,
    IUnknown* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* webViewEnvironmentCreatedHandler)
{
    WriteLog(L"[hooks] CreateWebViewEnvironmentWithOptionsInternal() вызван игрой (статически слинкованный загрузчик)");

    auto wrapped = WrapEnvironmentHandler(webViewEnvironmentCreatedHandler);

    return True_CreateWebViewEnvironmentWithOptionsInternal(
        checkRunningInstance, runtimeType, userDataFolder, environmentOptions, wrapped.Get());
}

// Once-only флаг: хук №1 либо ещё не установлен, либо уже установлен.
// Нужен, чтобы не поставить его дважды (и чтобы Hooked_LoadLibraryExW знал,
// когда можно перестать проверять новые модули).
static std::atomic<bool> g_environmentHookInstalled{false};

static void InstallEnvironmentHook(void* target, WebView2EntryPointKind kind)
{
    MH_STATUS createStatus = (kind == WebView2EntryPointKind::Public)
        ? MH_CreateHook(target, &Hooked_CreateCoreWebView2EnvironmentWithOptions,
                         reinterpret_cast<void**>(&True_CreateCoreWebView2EnvironmentWithOptions))
        : MH_CreateHook(target, &Hooked_CreateWebViewEnvironmentWithOptionsInternal,
                         reinterpret_cast<void**>(&True_CreateWebViewEnvironmentWithOptionsInternal));

    if (createStatus != MH_OK) {
        WriteLog(L"[hooks] MH_CreateHook() для хука №1 не удался");
        return;
    }
    if (MH_EnableHook(target) != MH_OK) {
        WriteLog(L"[hooks] MH_EnableHook() для хука №1 не удался");
        return;
    }

    g_environmentHookInstalled = true;
    WriteLog(L"[hooks] Хук №1 установлен успешно (" +
        std::wstring(kind == WebView2EntryPointKind::Public
            ? L"публичный CreateCoreWebView2EnvironmentWithOptions"
            : L"внутренний CreateWebViewEnvironmentWithOptionsInternal") + L")");
}

// =====================================================================
//  Хук №1, окончательная версия: сама GetProcAddress.
//
//  Статический анализ code-terraform.exe (дизассемблирование .text,
//  поиск xref на строку "EmbeddedBrowserWebView.dll" через RIP-relative
//  LEA) показал ТОЧНУЮ картину: игра сама строит путь к рантайму (реестр
//  + проверка файла через GetFileAttributesW/CreateFileW), сама грузит
//  его через самый обычный, ничем не подмененный kernel32!LoadLibraryW
//  (call QWORD PTR [rip+...] на реальный IAT-слот LoadLibraryW, VA
//  0x1406b3c19), и СРАЗУ следующей инструкцией вызывает
//  GetProcAddress(hModule, "CreateWebViewEnvironmentWithOptionsInternal")
//  (VA 0x1406b3c31 - буквально нашли эту строку как аргумент). Результат
//  GetProcAddress кладётся в регистр и вызывается напрямую - НЕ через
//  IAT, поэтому его никакой инжект-хук IAT-функций в принципе не поймает.
//
//  Отсюда следует вывод: хукать нужно не "момент загрузки модуля"
//  (LoadLibrary*/LdrLoadDll - что и объясняет, почему все предыдущие
//  попытки с ними ни к чему не приводили - сама загрузка модуля тут ни
//  при чём, дело было в GetProcAddress), а саму функцию GetProcAddress:
//  когда игра попросит именно это имя - подменяем ВОЗВРАЩАЕМЫЙ указатель
//  на наш враппер. Дальше игра сама (ничего не подозревая) вызывает наш
//  код вместо настоящего, и с гонками покончено: подмена происходит
//  синхронно, в тот же самый момент, когда игра узнаёт адрес функции -
//  раньше, чем она успевает его вызвать.
// =====================================================================

using GetProcAddress_t = FARPROC(WINAPI*)(HMODULE hModule, LPCSTR lpProcName);
static GetProcAddress_t True_GetProcAddress = nullptr;

static FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    FARPROC real = True_GetProcAddress(hModule, lpProcName);

    // lpProcName может быть "ординалом" (числом, а не строкой) - в этом
    // случае указатель не валиден как C-строка, и strcmp на нём упадёт.
    // Ординалы передаются как значения < 0x10000 (см. макрос IS_INTRESOURCE).
    if (real && !g_environmentHookInstalled.load() && !IS_INTRESOURCE(lpProcName)) {
        if (std::strcmp(lpProcName, "CreateCoreWebView2EnvironmentWithOptions") == 0) {
            True_CreateCoreWebView2EnvironmentWithOptions =
                reinterpret_cast<CreateCoreWebView2EnvironmentWithOptions_t>(real);
            g_environmentHookInstalled = true;
            WriteLog(L"[hooks] GetProcAddress запросила публичный CreateCoreWebView2EnvironmentWithOptions - подменяем указатель");
            return reinterpret_cast<FARPROC>(&Hooked_CreateCoreWebView2EnvironmentWithOptions);
        }
        if (std::strcmp(lpProcName, "CreateWebViewEnvironmentWithOptionsInternal") == 0) {
            True_CreateWebViewEnvironmentWithOptionsInternal =
                reinterpret_cast<CreateWebViewEnvironmentWithOptionsInternal_t>(real);
            g_environmentHookInstalled = true;
            WriteLog(L"[hooks] GetProcAddress запросила внутренний CreateWebViewEnvironmentWithOptionsInternal - подменяем указатель");
            return reinterpret_cast<FARPROC>(&Hooked_CreateWebViewEnvironmentWithOptionsInternal);
        }
    }

    return real;
}

// =====================================================================
//  Установка хуков
// =====================================================================

namespace Hooks {

// =====================================================================
//  ПОЧЕМУ хук на GetProcAddress тоже не сработал сразу же, хотя точка
//  перехвата была верно определена статическим анализом:
//
//  Раньше вся установка хуков (включая эту) уходила в отдельный поток,
//  создаваемый из DllMain (см. комментарий там про loader lock). Но это
//  само по себе создавало ГОНКУ ПОТОКОВ верхнего уровня: инжектор делает
//  CREATE_SUSPENDED -> инжект DLL -> ResumeThread. Пока наш новый поток
//  только создаётся и ОС его планирует, ResumeThread уже мог случиться -
//  и основной поток игры добегал до LoadLibraryW+GetProcAddress БЫСТРЕЕ,
//  чем наш фоновый поток успевал вызвать MH_Initialize и поставить хук.
//  Дело было не в ТОЧКЕ перехвата, а в ТАЙМИНГЕ его установки.
//
//  Поэтому теперь хук на GetProcAddress ставится не в фоновом потоке, а
//  СИНХРОННО прямо в DllMain (см. InstallEarly ниже), до того как DllMain
//  вернёт управление - а значит и до того, как LoadLibraryW в инжекторе
//  завершится и инжектор успеет вызвать ResumeThread. Это гарантирует:
//  хук физически не может не успеть - основной поток игры в этот момент
//  ещё не запущен вообще.
//
//  MH_Initialize и MH_CreateHook/MH_EnableHook безопасны внутри DllMain
//  (под loader lock): они не грузят новые DLL и не блокируются - в
//  отличие от Sleep/WaitFor*/LoadLibrary, которых тут по-прежнему нет.
// =====================================================================

bool InstallEarly()
{
    if (MH_Initialize() != MH_OK) {
        WriteLog(L"[hooks] MH_Initialize() не удался");
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* getProcAddr = kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, "GetProcAddress")) : nullptr;

    if (!getProcAddr ||
        MH_CreateHook(getProcAddr, &Hooked_GetProcAddress,
                      reinterpret_cast<void**>(&True_GetProcAddress)) != MH_OK ||
        MH_EnableHook(getProcAddr) != MH_OK) {
        WriteLog(L"[hooks] Не удалось поставить хук на GetProcAddress");
        return false;
    }

    WriteLog(L"[hooks] Хук на GetProcAddress установлен синхронно в DllMain (до ResumeThread)");
    return true;
}

void Install()
{
    // Эта часть по-прежнему в фоновом потоке - тут можно спокойно ждать
    // и обращаться к Toolhelp32, гонка уже устранена в InstallEarly().

    // На случай attach-режима (инжект в УЖЕ запущенный процесс, где
    // рантайм мог быть загружен ещё до нас) - проверяем сразу.
    if (!g_environmentHookInstalled.load()) {
        void* target = nullptr;
        WebView2EntryPointKind kind = FindWebView2ExportingModule(&target);
        if (kind != WebView2EntryPointKind::None) {
            InstallEnvironmentHook(target, kind);
            return;
        }
    }

    WriteLog(L"[hooks] Ждём срабатывания хука GetProcAddress...");

    // Диагностический предохранитель: если через 60 секунд хук №1 так и
    // не встал - выгружаем список модулей.
    for (int i = 0; i < 600; ++i) {
        if (g_environmentHookInstalled.load()) return;
        Sleep(100);
    }

    if (!g_environmentHookInstalled.load()) {
        WriteLog(L"[hooks] За 60 секунд хук №1 так и не встал, выходим");
        DumpLoadedModules();
    }
}

} // namespace Hooks
