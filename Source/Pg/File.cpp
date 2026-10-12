#include "Pg/File.h"

#ifdef _WIN32
#include <Windows.h>
#include <vector>
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
