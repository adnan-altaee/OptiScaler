#pragma once

#include "Config.h"
#include "State.h"

#include <proxies/Ntdll_Proxy.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// Custom frame generation providers, ported from upstream OptiScaler 941ce6a ("Add the ability to load custom DLSSG
// dlls"), frame generation part only (no custom upscalers, no plugin API):
//   custom/<name>/dlssg    nvngx_dlssg replacement, an FG Nvngx entry
// The dll can have any name. When the folder has more dlls, nvngx_dlssg.dll is used, else the first one.
// Set it with FGNvngxReplacement=<name> in OptiScaler.ini or pick it in the menu's FG Nvngx list.
namespace CustomProviders
{
inline std::filesystem::path Root()
{
    return std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"custom";
}

inline std::filesystem::path Folder(const std::string& name) { return Root() / string_to_wstring(name); }

// The dll in custom/<name>/<kind>, empty when there's none
inline std::filesystem::path DllPath(const std::string& name, const wchar_t* kind,
                                     std::initializer_list<const wchar_t*> standardNames)
{
    auto folder = Folder(name) / kind;
    std::error_code error;

    for (auto standardName : standardNames)
    {
        if (std::filesystem::exists(folder / standardName, error))
            return folder / standardName;
    }

    for (auto& entry : std::filesystem::directory_iterator(folder, error))
    {
        if (_wcsicmp(entry.path().extension().c_str(), L".dll") == 0)
            return entry.path();
    }

    return {};
}

inline std::filesystem::path DlssgDllPath(const std::string& name)
{
    return DllPath(name, L"dlssg", { L"nvngx_dlssg.dll" });
}

// nullptr when the path is empty or the dll can't be loaded
inline HMODULE LoadDll(const std::string& name, const std::filesystem::path& path)
{
    auto dll = path.empty() ? nullptr : NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), NULL, NULL);

    if (dll == nullptr)
    {
        LOG_ERROR("Can't load custom {} from {}", name, wstring_to_string(Folder(name).wstring()));
        return nullptr;
    }

    LOG_INFO("Loaded custom {} from {}", name, wstring_to_string(path.wstring()));
    return dll;
}

struct Names
{
    std::vector<std::string> dlssg;
};

// Scanned once
inline const Names& Scan()
{
    static const auto names = []
    {
        Names found;
        std::error_code error;

        for (auto& entry : std::filesystem::directory_iterator(Root(), error))
        {
            auto name = wstring_to_string(entry.path().filename().wstring());

            if (!DlssgDllPath(name).empty())
                found.dlssg.push_back(name);
        }

        return found;
    }();

    return names;
}

// Folder names aren't case sensitive
inline bool Contains(const std::vector<std::string>& names, const std::string& name)
{
    return std::ranges::any_of(names, [&name](const std::string& n) { return _stricmp(n.c_str(), name.c_str()) == 0; });
}

inline bool IsDlssg(const std::string& name) { return Contains(Scan().dlssg, name); }

// FGNvngxReplacement from the ini as a custom folder with dlssg, called once the Opti folder is known
inline void ResolveFGNvngx()
{
    auto& name = State::Instance().fgNvngxCustom;

    if (name.empty())
        return;

    if (!IsDlssg(name))
    {
        LOG_WARN("FGNvngxReplacement={} isn't a replacement or a folder with dlssg in {}", name,
                 wstring_to_string(Root().wstring()));
        name.clear();
        Config::Instance()->FGNvngxReplacement.reset();
        return;
    }

    LOG_INFO("FGNvngxReplacement: custom {} from {}", name, wstring_to_string(DlssgDllPath(name).wstring()));
}
} // namespace CustomProviders
