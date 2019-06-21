
#include <cppfs/windows/FileNameConversions.h>

#include <windows.h>


namespace cppfs
{

namespace convert
{

std::wstring utf8ToWideString(const std::string & utf8)
{
    int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);

    std::wstring wide(length, 0);
    if (!wide.empty())
    {
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wide.size());
    }

    return wide;
}

std::string wideToUtf8String(const std::wstring & wide)
{
    int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide.size(), nullptr, 0, nullptr, nullptr);

    std::string utf8(length, 0);
    if (!utf8.empty())
    {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide.size(), &utf8[0], length, nullptr, nullptr);
    }

    return utf8;
}

} // namespace convert
} // namespace cppfs
