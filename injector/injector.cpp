// Простейший инжектор DLL через классическую связку
// VirtualAllocEx + WriteProcessMemory + CreateRemoteThread(LoadLibraryW).
//
// Это не обязательный компонент - для тестов вполне достаточно Xenos
// или "Inject DLL" из Process Hacker, как и предполагалось в исходном
// плане. Этот инжектор просто удобен, когда не хочется держать под рукой
// сторонние программы.
//
// ВАЖНО про WebView2-игры (в частности Tauri): окружение WebView2
// создаётся практически сразу при старте игры, ещё до того как успевает
// показаться окно. Если внедрять DLL уже ПОСЛЕ запуска игры (когда меню
// уже видно) - момент создания environment уже прошёл, и наш хук ничего
// не поймает. Поэтому для таких игр нужно внедрять DLL ДО того, как игра
// начнёт выполняться - см. режим --launch ниже.
//
// Использование:
//   injector.exe <имя_процесса.exe> <путь_к_dll>
//       - подключиться к УЖЕ запущенному процессу (годится для игр без
//         WebView2 или если нужно довнедрить DLL повторно).
//
//   injector.exe --launch <путь_к_exe_игры> <путь_к_dll>
//       - запускает игру САМ, в приостановленном состоянии (CREATE_SUSPENDED),
//         внедряет DLL, и только потом даёт игре продолжить выполнение.
//         Это гарантирует, что DLL (и наши хуки) окажутся внутри процесса
//         до того, как игра успеет создать WebView2 environment.
//         Удобно для ярлыка/bat-файла с фиксированными путями.
//
//   injector.exe --launch-cmdline <путь_к_dll> <команда запуска игры...>
//       - то же самое, что --launch, но всё после пути к DLL склеивается
//         обратно в ОДНУ командную строку и передаётся в CreateProcess как
//         есть (а не как ровно два аргумента exe+dll). Нужно для Steam
//         Launch Options: там подставляется %command%, который может
//         содержать не только путь к exe, но и собственные параметры игры -
//         --launch этого не поддержит, а --launch-cmdline примет любое их
//         количество.

#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

static DWORD FindProcessId(const std::wstring& processName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(processName.c_str(), entry.szExeFile) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

// Общая логика внедрения DLL в уже открытый (HANDLE) процесс.
// Возвращает true, если LoadLibraryW в процессе вернул ненулевой модуль.
static bool InjectDll(HANDLE process, const std::wstring& dllPath)
{
    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        std::wcout << L"VirtualAllocEx не удался.\n";
        return false;
    }

    if (!WriteProcessMemory(process, remoteMem, dllPath.c_str(), bytes, nullptr)) {
        std::wcout << L"WriteProcessMemory не удался.\n";
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    // kernel32.dll грузится системным загрузчиком по одному и тому же
    // адресу во всех процессах в рамках одной сессии Windows, поэтому
    // адрес LoadLibraryW, полученный в НАШЕМ процессе, действителен и в
    // процессе игры. Это стандартная и давно известная техника инжекта.
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibraryAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibraryAddr, remoteMem, 0, nullptr);
    if (!thread) {
        std::wcout << L"CreateRemoteThread не удался.\n";
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(thread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    if (exitCode) {
        std::wcout << L"DLL успешно загружена (module handle: 0x" << std::hex << exitCode << L")\n";
    } else {
        std::wcout << L"LoadLibraryW вернул 0 - DLL не загрузилась.\n"
                       L"Проверьте: 1) битность DLL (x86/x64) совпадает с игрой; "
                       L"2) путь к DLL указан полностью и без опечаток.\n";
    }

    VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
    CloseHandle(thread);
    return exitCode != 0;
}

static int AttachMode(const std::wstring& processName, const std::wstring& dllPath)
{
    DWORD pid = FindProcessId(processName);
    if (!pid) {
        std::wcout << L"Процесс " << processName << L" не найден. Запущена ли игра?\n";
        return 1;
    }

    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!process) {
        std::wcout << L"Не удалось открыть процесс (запустите инжектор от имени администратора).\n";
        return 1;
    }

    bool ok = InjectDll(process, dllPath);
    CloseHandle(process);
    return ok ? 0 : 1;
}

static int LaunchMode(const std::wstring& exePath, const std::wstring& dllPath)
{
    std::filesystem::path p(exePath);
    std::wstring workingDir = p.has_parent_path() ? p.parent_path().wstring() : L"";

    // CreateProcessW может модифицировать буфер командной строки, поэтому
    // передаём её как изменяемый std::vector<wchar_t>, а не const-строку.
    std::wstring cmdLine = L"\"" + exePath + L"\"";
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CREATE_SUSPENDED: главный поток игры создаётся, но не начинает
    // выполняться, пока мы сами не вызовем ResumeThread. Это даёт нам
    // время внедрить DLL ДО того, как игра успеет что-либо сделать -
    // в частности, до того как она создаст WebView2 environment.
    BOOL created = CreateProcessW(
        exePath.c_str(),
        cmdLineBuf.data(),
        nullptr, nullptr, FALSE,
        CREATE_SUSPENDED,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &si, &pi);

    if (!created) {
        std::wcout << L"Не удалось запустить игру. Код ошибки: " << GetLastError() << L"\n";
        return 1;
    }

    std::wcout << L"Игра запущена в приостановленном состоянии (PID " << pi.dwProcessId
               << L"), внедряем DLL...\n";

    bool ok = InjectDll(pi.hProcess, dllPath);

    // Отпускаем игру - она начинает выполняться уже с нашей DLL внутри,
    // до того как успеет создать WebView2 environment.
    ResumeThread(pi.hThread);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok ? 0 : 1;
}

// Склеивает argv[from..argc) обратно в одну командную строку для
// CreateProcess. CRT уже разобрал исходную строку на токены (учтя кавычки
// оригинала), поэтому здесь просто заново оборачиваем в кавычки те токены,
// где есть пробел - этого достаточно для путей и обычных аргументов игр.
static std::wstring JoinArgs(int argc, wchar_t** argv, int from)
{
    std::wstring result;
    for (int i = from; i < argc; ++i) {
        std::wstring arg = argv[i];
        bool needsQuotes = arg.empty() || arg.find(L' ') != std::wstring::npos;
        if (!result.empty()) result += L' ';
        result += needsQuotes ? (L"\"" + arg + L"\"") : arg;
    }
    return result;
}

static int LaunchCmdlineMode(const std::wstring& dllPath, int argc, wchar_t** argv, int cmdStartIndex)
{
    std::wstring cmdLine = JoinArgs(argc, argv, cmdStartIndex);
    if (cmdLine.empty()) {
        std::wcout << L"Не указана команда запуска игры (аргументы после пути к DLL).\n";
        return 1;
    }

    // Рабочая папка - как и в --launch, папка самого exe (первый токен
    // командной строки): некоторые игры рассчитывают, что CWD - это их
    // собственная папка установки, а не папка инжектора.
    std::wstring exeToken = argv[cmdStartIndex];
    if (exeToken.size() >= 2 && exeToken.front() == L'"' && exeToken.back() == L'"') {
        exeToken = exeToken.substr(1, exeToken.size() - 2);
    }
    std::filesystem::path p(exeToken);
    std::wstring workingDir = p.has_parent_path() ? p.parent_path().wstring() : L"";

    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // lpApplicationName = nullptr: путь к exe уже часть командной строки
    // (первый токен), пусть CreateProcess сам его выделит - это то, что
    // ожидает Steam при подстановке %command%.
    BOOL created = CreateProcessW(
        nullptr,
        cmdLineBuf.data(),
        nullptr, nullptr, FALSE,
        CREATE_SUSPENDED,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &si, &pi);

    if (!created) {
        std::wcout << L"Не удалось запустить игру. Код ошибки: " << GetLastError() << L"\n"
                   << L"Командная строка была: " << cmdLine << L"\n";
        return 1;
    }

    std::wcout << L"Игра запущена в приостановленном состоянии (PID " << pi.dwProcessId
               << L"), внедряем DLL...\n";

    bool ok = InjectDll(pi.hProcess, dllPath);
    ResumeThread(pi.hThread);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok ? 0 : 1;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 4 && std::wstring(argv[1]) == L"--launch") {
        return LaunchMode(argv[2], argv[3]);
    }

    if (argc >= 4 && std::wstring(argv[1]) == L"--launch-cmdline") {
        return LaunchCmdlineMode(argv[2], argc, argv, 3);
    }

    if (argc == 3) {
        return AttachMode(argv[1], argv[2]);
    }

    std::wcout
        << L"Использование:\n"
        << L"  injector.exe <имя_процесса.exe> <путь_к_dll>\n"
        << L"      - подключиться к уже запущенному процессу\n"
        << L"  injector.exe --launch <путь_к_exe_игры> <путь_к_dll>\n"
        << L"      - запустить игру самим инжектором и внедрить DLL до того,\n"
        << L"        как игра успеет создать WebView2 environment (рекомендуется)\n"
        << L"  injector.exe --launch-cmdline <путь_к_dll> <команда запуска игры...>\n"
        << L"      - то же самое, но команда запуска - это ВСЁ, что после пути\n"
        << L"        к DLL (для Steam Launch Options: %command%)\n";
    return 1;
}
