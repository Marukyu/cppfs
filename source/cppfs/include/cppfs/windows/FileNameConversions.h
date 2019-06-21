
#pragma once


#include <string>

namespace cppfs
{

/**
*  @brief
*    Utility functions UTF-8/UTF-16 string conversions
*/
namespace convert
{

std::wstring utf8ToWideString(const std::string & utf8);
std::string wideToUtf8String(const std::wstring & wide);

} // namespace convert
} // namespace cppfs
