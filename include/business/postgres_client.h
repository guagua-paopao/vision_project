#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct pg_conn;
struct pg_result;

namespace yolo11_server {

constexpr int PG_STEP_ERROR = -1;
constexpr int PG_STEP_DONE = 0;
constexpr int PG_STEP_ROW = 1;

class PostgresStatement;

class PostgresConnection final {
public:
    PostgresConnection() = default;
    ~PostgresConnection() noexcept;

    PostgresConnection(const PostgresConnection&) = delete;
    PostgresConnection& operator=(const PostgresConnection&) = delete;

    bool openFromEnvironment(const std::string& dsn_env, std::string& error);
    bool exec(const std::string& sql, std::string& error);
    std::unique_ptr<PostgresStatement> prepare(const std::string& sql, std::string& error);

    int changedRows() const noexcept { return changed_rows_; }
    const std::string& sqlState() const noexcept { return sql_state_; }
    const std::string& lastError() const noexcept { return last_error_; }

private:
    friend class PostgresStatement;
    void setResultStatus(pg_result* result);
    pg_conn* connection_ = nullptr;
    int changed_rows_ = 0;
    std::string sql_state_;
    std::string last_error_;
};

class PostgresStatement final {
public:
    PostgresStatement(PostgresConnection& connection, std::string sql, std::size_t parameter_count);
    ~PostgresStatement() noexcept;

    PostgresStatement(const PostgresStatement&) = delete;
    PostgresStatement& operator=(const PostgresStatement&) = delete;

    void bindText(int index, const std::string& value);
    void bindInt(int index, int value);
    void bindInt64(int index, long long value);
    void bindDouble(int index, double value);
    void bindNull(int index);

    int step();
    std::string columnText(int index) const;
    int columnInt(int index) const;
    long long columnInt64(int index) const;
    double columnDouble(int index) const;
    bool columnIsNull(int index) const;

private:
    void bind(int index, std::optional<std::string> value);
    PostgresConnection& connection_;
    std::string sql_;
    std::vector<std::optional<std::string>> parameters_;
    pg_result* result_ = nullptr;
    int next_row_ = 0;
    bool executed_ = false;
};

bool postgresSqlStateIsUniqueViolation(const PostgresConnection& connection) noexcept;

}  // namespace yolo11_server
