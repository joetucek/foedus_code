/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include <foedus/fs/filesystem.hpp>
#include <foedus/fs/path.hpp>
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

/**
 * @file tpch_headers.cpp
 * These testcases are quite special.
 * They invoke the compiler (gcc) for header independence and conformance to our C++11 policy.
 */

#define X_QUOTE(str) #str
#define X_EXPAND_AND_QUOTE(str) X_QUOTE(str)
// -DFOEDUS_CORE_SRC_ROOT/-DTINYXML2_SRC_ROOT is given just for this testcase
const char *SRC_ROOT_PATH = X_EXPAND_AND_QUOTE(FOEDUS_CORE_SRC_ROOT);
const char *TINYXML2_ROOT_PATH = X_EXPAND_AND_QUOTE(TINYXML2_SRC_ROOT);
#undef X_EXPAND_AND_QUOTE
#undef X_QUOTE

namespace foedus {
DEFINE_TEST_CASE_PACKAGE(HeadersTest, foedus);

bool ends_with(const std::string& str, const std::string& pattern) {
    return str.find(pattern) != str.npos;
}

void write_dummy_cpp(const fs::Path &header) {
    std::string inc = header.string().substr((std::string(SRC_ROOT_PATH) + "/include/").size());
    std::ofstream stream;
    stream.open("dummy.cpp", std::ios_base::out | std::ios_base::trunc);
    EXPECT_TRUE(stream.is_open());
    stream << "#include <" << inc << ">" << std::endl;
    stream << "int main() { return 0; }" << std::endl;
    stream.flush();
    stream.close();
}

void get_headers_recursive(std::vector< fs::Path > *result, const fs::Path& folder) {
    for (const fs::Path& child : folder.child_paths()) {
        if (fs::is_directory(child)) {
            get_headers_recursive(result, child);
        } else {
            if (ends_with(child.string(), ".hpp")) {
                result->emplace_back(child);
            }
        }
    }
}

void get_all_headers(std::vector< fs::Path > *result) {
    fs::Path root(SRC_ROOT_PATH);
    root /= "include";
    get_headers_recursive(result, root);
}

TEST(HeadersTest, CheckCompiler) {
    EXPECT_EQ(0, std::system("g++ --version"));
}

TEST(HeadersTest, IndependenceCXX11) {
    // check if all headers are compilable by itself, C++11 on.
    std::vector< fs::Path > headers;
    get_all_headers(&headers);
    std::cout << "Have " << headers.size() << " headers to check" << std::endl;
    for (const fs::Path& header : headers) {
        std::cout << "Checking " << header << std::endl;
        write_dummy_cpp(header);
        std::stringstream cmd;
        cmd << "g++ -W -std=c++11 -I" << SRC_ROOT_PATH << "/include -I" << TINYXML2_ROOT_PATH
            << " dummy.cpp";
        EXPECT_EQ(0, std::system(cmd.str().c_str())) << header.string();
    }
}

TEST(HeadersTest, IndependenceCXX98) {
    // check if all _public_ headers are compilable by itself without C++11.
    std::vector< fs::Path > headers;
    get_all_headers(&headers);
    std::cout << "Have " << headers.size() << " headers to check" << std::endl;
    for (const fs::Path& header : headers) {
        if (ends_with(header.string(), "impl.hpp")) {
            std::cout << "Skipped private header: " << header << std::endl;
            continue;
        }
        std::cout << "Checking " << header << std::endl;
        write_dummy_cpp(header);
        std::stringstream cmd;
        cmd << "g++ -W -std=c++03 -I" << SRC_ROOT_PATH << "/include -I" << TINYXML2_ROOT_PATH
            << " -DNO_FOEDUS_CXX11_WARNING dummy.cpp";
        EXPECT_EQ(0, std::system(cmd.str().c_str())) << header.string();
    }
}

}  // namespace foedus
