#include "Pg/File.h"

#include <fstream>

#ifdef _WIN32
#include <Windows.h>
#include <shobjidl.h>
#include <cstring>
#include <vector>
#else
#include <sys/wait.h>
#include <array>
#include <cstdio>
#include <format>
#endif

std::expected<std::filesystem::path, std::string> ExecutableDirectory()
{
#ifdef _WIN32
    std::vector<wchar_t> Path(MAX_PATH);
    for (;;)
    {
        const DWORD Length = GetModuleFileNameW(nullptr, Path.data(), DWORD(Path.size()));
        if (Length == 0)
        {
            return std::unexpected("Cannot locate executable: Windows error " + std::to_string(GetLastError()));
        }

        if (Length < Path.size())
        {
            return std::filesystem::path(Path.data(), Path.data() + Length).parent_path();
        }

        Path.resize(Path.size() * 2);
    }
#else
    std::error_code Error;
    const std::filesystem::path Path = std::filesystem::read_symlink("/proc/self/exe", Error);
    if (Error)
    {
        return std::unexpected("Cannot locate executable: " + Error.message());
    }

    return Path.parent_path();
#endif
}

std::optional<std::filesystem::path> PickFolder(const char* const Title)
{
#ifdef _WIN32
    const HRESULT Initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::optional<std::filesystem::path> Result;
    IFileOpenDialog* Dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog))))
    {
        FILEOPENDIALOGOPTIONS Options = 0;
        Dialog->GetOptions(&Options);
        Dialog->SetOptions(Options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        const std::wstring WideTitle(Title, Title + std::strlen(Title));
        Dialog->SetTitle(WideTitle.c_str());
        IShellItem* Item = nullptr;
        if (SUCCEEDED(Dialog->Show(nullptr)) && SUCCEEDED(Dialog->GetResult(&Item)))
        {
            PWSTR Path = nullptr;
            if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Path)))
            {
                Result = std::filesystem::path(Path);
                CoTaskMemFree(Path);
            }

            Item->Release();
        }

        Dialog->Release();
    }

    if (SUCCEEDED(Initialized))
    {
        CoUninitialize();
    }

    return Result;
#else
    // The shell exits with 127 when the picker is not installed, so the next one gets a turn; any other status is the user's answer.
    constexpr int CommandNotFound = 127;
    for (const char* const Format : { "zenity --file-selection --directory --title='{}' 2>/dev/null", "kdialog --getexistingdirectory --title '{}' 2>/dev/null" })
    {
        FILE* const Pipe = popen(std::vformat(Format, std::make_format_args(Title)).c_str(), "r");
        if (!Pipe)
        {
            continue;
        }

        std::string Output;
        std::array<char, 256> Buffer;
        while (std::fgets(Buffer.data(), int(Buffer.size()), Pipe))
        {
            Output += Buffer.data();
        }

        const int Status = pclose(Pipe);
        if (WIFEXITED(Status) && WEXITSTATUS(Status) == CommandNotFound)
        {
            continue;
        }

        while (!Output.empty() && (Output.back() == '\n' || Output.back() == '\r'))
        {
            Output.pop_back();
        }

        if (!WIFEXITED(Status) || WEXITSTATUS(Status) != 0 || Output.empty())
        {
            return std::nullopt;
        }

        return std::filesystem::path(Output);
    }

    return std::nullopt;
#endif
}

void BackUpFile(const std::filesystem::path& Path)
{
    std::error_code Error;
    if (std::filesystem::is_regular_file(Path, Error))
    {
        std::filesystem::path Backup = Path;
        Backup += ".bak";
        std::filesystem::remove(Backup, Error);
        std::filesystem::rename(Path, Backup, Error);
    }
}

bool WriteFileText(const std::filesystem::path& Path, const std::string_view Text)
{
    BackUpFile(Path);
    std::ofstream File(Path, std::ios::binary | std::ios::trunc);
    File.write(Text.data(), std::streamsize(Text.size()));
    return bool(File);
}
