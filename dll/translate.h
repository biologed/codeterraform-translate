#pragma once
#include <string>

namespace Translate {

    // Загружает translations.json из папки DLL в память.
    // Вызывается автоматически при первом обращении к BuildBootstrapScript(),
    // а также вручную из WebViewHooks::ReloadTranslations() (хоткей F11).
    void Reload();

    // Возвращает готовый JS-код (в виде "широкой" UTF-16 строки, как того
    // требует ExecuteScript/AddScriptToExecuteOnDocumentCreated), который:
    //   1) проходит по document.body и заменяет текст по словарю;
    //   2) ставит MutationObserver, чтобы переводить и то, что появится
    //      на странице позже (Tauri/SPA перерисовывает контент динамически).
    std::wstring BuildBootstrapScript();

}
