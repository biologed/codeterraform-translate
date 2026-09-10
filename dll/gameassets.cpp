#include "gameassets.h"
#include "hooks.h"

#include <windows.h>
#include <brotli/decode.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

    struct Blob {
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

    std::unordered_map<std::string, Blob> g_assets;   // "assets/x.js" -> сжатые байты
    std::once_flag g_loadOnce;
    size_t g_loadedCount = 0;

    // Одна запись таблицы ассетов Tauri: два среза (имя и данные) подряд.
    // Указатели - обычные адреса в уже загруженном образе, править их не
    // нужно: за нас это сделал загрузчик Windows.
    struct AssetEntry {
        const char* name;
        size_t nameLen;
        const uint8_t* data;
        size_t dataLen;
    };

    struct Section {
        const uint8_t* begin = nullptr;
        const uint8_t* end = nullptr;
        bool contains(const void* p, size_t len) const {
            auto q = static_cast<const uint8_t*>(p);
            return q >= begin && q + len <= end;
        }
    };

    // Находит секцию по имени в образе, УЖЕ загруженном в память.
    bool FindSection(const char* wanted, Section& out)
    {
        auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        if (!base) return false;

        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        auto section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            char name[9] = {};
            memcpy(name, section->Name, 8);
            if (strcmp(name, wanted) != 0) continue;

            out.begin = base + section->VirtualAddress;
            out.end = out.begin + section->Misc.VirtualSize;
            return true;
        }
        return false;
    }

    bool ValidEntry(const AssetEntry& e, const Section& rdata)
    {
        if (e.nameLen == 0 || e.nameLen > 512) return false;
        if (e.dataLen == 0 || e.dataLen > (256u << 20)) return false;
        if (!rdata.contains(e.name, e.nameLen)) return false;
        if (!rdata.contains(e.data, e.dataLen)) return false;
        if (e.name[0] != '/') return false;                 // путь ассета
        for (size_t i = 0; i < e.nameLen; ++i) {
            if (static_cast<unsigned char>(e.name[i]) < 0x20) return false;
        }
        return true;
    }

    const uint8_t* FindBytes(const uint8_t* begin, const uint8_t* end,
                             const void* needle, size_t len)
    {
        if (static_cast<size_t>(end - begin) < len) return nullptr;
        for (const uint8_t* p = begin; p <= end - len; ++p) {
            if (memcmp(p, needle, len) == 0) return p;
        }
        return nullptr;
    }

    void LoadImpl()
    {
        Section rdata;
        if (!FindSection(".rdata", rdata)) {
            WriteLog(L"[gameassets] Секция .rdata в образе игры не найдена");
            return;
        }

        // Опорная точка - строка "/index.html": она есть в любой сборке
        // Tauri. Находим её саму, затем - запись таблицы, которая на неё
        // ссылается (указатель + длина этой же строки рядом).
        static const char kAnchor[] = "/index.html";
        const size_t anchorLen = sizeof(kAnchor) - 1;
        const uint8_t* anchor = FindBytes(rdata.begin, rdata.end, kAnchor, anchorLen);
        if (!anchor) {
            WriteLog(L"[gameassets] Опорная строка \"/index.html\" в .rdata не найдена - "
                     L"похоже, это не Tauri-сборка");
            return;
        }

        struct { const void* ptr; size_t len; } needle{ anchor, anchorLen };
        const uint8_t* hit = rdata.begin;
        const AssetEntry* start = nullptr;
        while ((hit = FindBytes(hit, rdata.end, &needle, sizeof(needle))) != nullptr) {
            auto candidate = reinterpret_cast<const AssetEntry*>(hit);
            if (rdata.contains(candidate, sizeof(AssetEntry)) && ValidEntry(*candidate, rdata)) {
                start = candidate;
                break;
            }
            hit += sizeof(void*);
        }
        if (!start) {
            WriteLog(L"[gameassets] Ссылка на \"/index.html\" не нашлась - формат таблицы "
                     L"ассетов, видимо, изменился");
            return;
        }

        // От найденной записи идём в обе стороны, пока записи валидны.
        auto add = [](const AssetEntry* e) {
            std::string name(e->name, e->nameLen);
            if (!name.empty() && name[0] == '/') name.erase(0, 1);
            g_assets.emplace(std::move(name), Blob{ e->data, e->dataLen });
        };

        for (const AssetEntry* e = start; rdata.contains(e, sizeof(*e)) && ValidEntry(*e, rdata); ++e) {
            add(e);
        }
        for (const AssetEntry* e = start - 1;
             e >= reinterpret_cast<const AssetEntry*>(rdata.begin)
                 && rdata.contains(e, sizeof(*e)) && ValidEntry(*e, rdata);
             --e) {
            add(e);
        }

        g_loadedCount = g_assets.size();
        WriteLog(L"[gameassets] Ассетов игры найдено в exe: " + std::to_wstring(g_loadedCount));
    }

    // Ассеты сжаты brotli. Распакованный размер заранее неизвестен, поэтому
    // распаковываем потоково, наращивая буфер.
    bool BrotliDecompress(const uint8_t* data, size_t size, std::string& out)
    {
        BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
        if (!state) return false;

        out.clear();
        out.reserve(size * 4);

        std::vector<uint8_t> chunk(256 * 1024);
        const uint8_t* next_in = data;
        size_t avail_in = size;
        BrotliDecoderResult result = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;

        while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT
               || result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
            uint8_t* next_out = chunk.data();
            size_t avail_out = chunk.size();
            result = BrotliDecoderDecompressStream(state, &avail_in, &next_in,
                                                   &avail_out, &next_out, nullptr);
            out.append(reinterpret_cast<const char*>(chunk.data()), chunk.size() - avail_out);

            if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && avail_in == 0) break;
        }

        bool ok = (result == BROTLI_DECODER_RESULT_SUCCESS);
        BrotliDecoderDestroyInstance(state);
        return ok;
    }

} // namespace

namespace GameAssets {

size_t Load()
{
    std::call_once(g_loadOnce, LoadImpl);
    return g_loadedCount;
}

bool Read(const std::wstring& path, std::string& out)
{
    Load();

    std::string key(path.begin(), path.end());     // пути ассетов - ASCII
    if (!key.empty() && key[0] == '/') key.erase(0, 1);

    auto it = g_assets.find(key);
    if (it == g_assets.end()) return false;

    // Часть ассетов (шрифты, картинки) лежит несжатой - для них
    // распаковка не удастся, и это нормально: отдаём как есть.
    if (BrotliDecompress(it->second.data, it->second.size, out)) return true;

    out.assign(reinterpret_cast<const char*>(it->second.data), it->second.size);
    return true;
}

} // namespace GameAssets
