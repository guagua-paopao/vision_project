#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

inline int requireDisposablePostgresTestDatabase() {
    const char* test_dsn = std::getenv("YOLO11_TEST_POSTGRES_DSN");
    const char* allow = std::getenv("YOLO11_ALLOW_DESTRUCTIVE_POSTGRES_TESTS");
    if (!test_dsn || !*test_dsn || !allow || std::string(allow) != "1") {
        std::cout << "SKIP: disposable PostgreSQL tests require YOLO11_TEST_POSTGRES_DSN "
                     "and YOLO11_ALLOW_DESTRUCTIVE_POSTGRES_TESTS=1\n";
        return 77;
    }
    const char* production_dsn = std::getenv("YOLO11_POSTGRES_DSN");
    if (production_dsn && *production_dsn && std::string(production_dsn) == test_dsn) {
        std::cerr << "FAIL: test and production PostgreSQL DSNs must be different\n";
        return 1;
    }
    return 0;
}
