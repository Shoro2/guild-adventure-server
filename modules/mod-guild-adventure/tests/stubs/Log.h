// Test stub of AzerothCore's Log.h: every LOG_* line is formatted and kept in TestLog() for the checks.
#ifndef GA_TEST_STUB_LOG_H
#define GA_TEST_STUB_LOG_H

#include "StringFormat.h"
#include <string>
#include <vector>

struct TestLogLine
{
    std::string level;
    std::string text;
};

std::vector<TestLogLine>& TestLog();

#define GA_TEST_LOG(level, filter, ...) TestLog().push_back({ level, Acore::StringFormat(__VA_ARGS__) })
#define LOG_TRACE(filter, ...) GA_TEST_LOG("TRACE", filter, __VA_ARGS__)
#define LOG_DEBUG(filter, ...) GA_TEST_LOG("DEBUG", filter, __VA_ARGS__)
#define LOG_INFO(filter, ...) GA_TEST_LOG("INFO", filter, __VA_ARGS__)
#define LOG_WARN(filter, ...) GA_TEST_LOG("WARN", filter, __VA_ARGS__)
#define LOG_ERROR(filter, ...) GA_TEST_LOG("ERROR", filter, __VA_ARGS__)

#endif
