// Test stub of AzerothCore's StringFormat.h on top of std::format (compile-time checked like the fmt original).
#ifndef GA_TEST_STUB_STRING_FORMAT_H
#define GA_TEST_STUB_STRING_FORMAT_H

#include <format>
#include <string>
#include <utility>

namespace Acore
{
    template <typename... Args>
    std::string StringFormat(std::format_string<Args...> fmt, Args&&... args)
    {
        return std::format(fmt, std::forward<Args>(args)...);
    }
}

#endif
