#include "business/postgres_client.h"

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>

#include <libpq-fe.h>

namespace yolo11_server {
namespace {

std::pair<std::string, std::size_t> positionalSql(const std::string& sql) {
    std::string result;
    result.reserve(sql.size() + 16);
    std::size_t parameter = 0;
    bool single_quote = false;
    for (std::size_t index = 0; index < sql.size(); ++index) {
        const char ch = sql[index];
        if (ch == '\'' && (index == 0 || sql[index - 1] != '\\')) single_quote = !single_quote;
        if (ch == '?' && !single_quote) {
            result += '$';
            result += std::to_string(++parameter);
        }
        else {
            result += ch;
        }
    }
    return { std::move(result), parameter };
}

std::string trimPostgresError(const char* value) {
    std::string result = value ? value : "PostgreSQL operation failed";
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
    return result;
}

}  // namespace

PostgresConnection::~PostgresConnection() noexcept {
    if (connection_) PQfinish(reinterpret_cast<PGconn*>(connection_));
}

bool PostgresConnection::openFromEnvironment(const std::string& dsn_env, std::string& error) {
    error.clear();
    if (connection_) {
        PQfinish(reinterpret_cast<PGconn*>(connection_));
        connection_ = nullptr;
    }
    if (dsn_env.empty()) {
        error = "PostgreSQL DSN environment-variable name is empty";
        return false;
    }
    const char* dsn = std::getenv(dsn_env.c_str());
    if (!dsn || !*dsn) {
        error = "PostgreSQL DSN environment variable is not configured: " + dsn_env;
        return false;
    }
    PGconn* raw = PQconnectdb(dsn);
    connection_ = reinterpret_cast<pg_conn*>(raw);
    if (!raw || PQstatus(raw) != CONNECTION_OK) {
        error = trimPostgresError(raw ? PQerrorMessage(raw) : "PQconnectdb returned null");
        last_error_ = error;
        return false;
    }
    PGresult* result = PQexec(raw, "SET client_encoding TO 'UTF8'; SET TIME ZONE 'UTC';");
    const bool ok = result && PQresultStatus(result) == PGRES_COMMAND_OK;
    if (!ok) error = trimPostgresError(result ? PQresultErrorMessage(result) : PQerrorMessage(raw));
    if (result) PQclear(result);
    last_error_ = error;
    return ok;
}

void PostgresConnection::setResultStatus(pg_result* opaque) {
    PGresult* result = reinterpret_cast<PGresult*>(opaque);
    changed_rows_ = 0;
    sql_state_.clear();
    last_error_.clear();
    if (!result) {
        last_error_ = connection_ ? trimPostgresError(PQerrorMessage(reinterpret_cast<PGconn*>(connection_)))
                                  : "PostgreSQL connection is unavailable";
        return;
    }
    const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
    if (state) sql_state_ = state;
    const char* tuples = PQcmdTuples(result);
    if (tuples && *tuples) {
        try { changed_rows_ = std::stoi(tuples); }
        catch (...) { changed_rows_ = 0; }
    }
    const ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        last_error_ = trimPostgresError(PQresultErrorMessage(result));
    }
}

bool PostgresConnection::exec(const std::string& sql, std::string& error) {
    error.clear();
    if (!connection_) {
        error = "PostgreSQL connection is unavailable";
        return false;
    }
    PGresult* result = PQexec(reinterpret_cast<PGconn*>(connection_), sql.c_str());
    setResultStatus(reinterpret_cast<pg_result*>(result));
    const bool ok = result && (PQresultStatus(result) == PGRES_COMMAND_OK ||
                               PQresultStatus(result) == PGRES_TUPLES_OK);
    if (!ok) error = last_error_;
    if (result) PQclear(result);
    return ok;
}

std::unique_ptr<PostgresStatement> PostgresConnection::prepare(
    const std::string& sql,
    std::string& error
) {
    error.clear();
    if (!connection_) {
        error = "PostgreSQL connection is unavailable";
        return {};
    }
    auto converted = positionalSql(sql);
    return std::make_unique<PostgresStatement>(*this, std::move(converted.first), converted.second);
}

PostgresStatement::PostgresStatement(
    PostgresConnection& connection,
    std::string sql,
    std::size_t parameter_count
) : connection_(connection), sql_(std::move(sql)), parameters_(parameter_count) {}

PostgresStatement::~PostgresStatement() noexcept {
    if (result_) PQclear(reinterpret_cast<PGresult*>(result_));
}

void PostgresStatement::bind(int index, std::optional<std::string> value) {
    if (index <= 0 || static_cast<std::size_t>(index) > parameters_.size()) return;
    parameters_[static_cast<std::size_t>(index - 1)] = std::move(value);
}

void PostgresStatement::bindText(int index, const std::string& value) { bind(index, value); }
void PostgresStatement::bindInt(int index, int value) { bind(index, std::to_string(value)); }
void PostgresStatement::bindInt64(int index, long long value) { bind(index, std::to_string(value)); }
void PostgresStatement::bindDouble(int index, double value) {
    std::ostringstream output;
    output << std::setprecision(17) << value;
    bind(index, output.str());
}
void PostgresStatement::bindNull(int index) { bind(index, std::nullopt); }

int PostgresStatement::step() {
    if (!executed_) {
        executed_ = true;
        std::vector<const char*> values(parameters_.size(), nullptr);
        for (std::size_t index = 0; index < parameters_.size(); ++index) {
            if (parameters_[index]) values[index] = parameters_[index]->c_str();
        }
        PGresult* raw = PQexecParams(
            reinterpret_cast<PGconn*>(connection_.connection_), sql_.c_str(),
            static_cast<int>(values.size()), nullptr, values.data(), nullptr, nullptr, 0);
        result_ = reinterpret_cast<pg_result*>(raw);
        connection_.setResultStatus(result_);
        if (!raw) return PG_STEP_ERROR;
        const ExecStatusType status = PQresultStatus(raw);
        if (status == PGRES_COMMAND_OK) return PG_STEP_DONE;
        if (status != PGRES_TUPLES_OK) return PG_STEP_ERROR;
    }
    PGresult* result = reinterpret_cast<PGresult*>(result_);
    if (!result || next_row_ >= PQntuples(result)) return PG_STEP_DONE;
    ++next_row_;
    return PG_STEP_ROW;
}

std::string PostgresStatement::columnText(int index) const {
    PGresult* result = reinterpret_cast<PGresult*>(result_);
    const int row = next_row_ - 1;
    if (!result || row < 0 || index < 0 || index >= PQnfields(result) || PQgetisnull(result, row, index)) {
        return {};
    }
    return PQgetvalue(result, row, index);
}

int PostgresStatement::columnInt(int index) const {
    try { return std::stoi(columnText(index)); }
    catch (...) { return 0; }
}

long long PostgresStatement::columnInt64(int index) const {
    try { return std::stoll(columnText(index)); }
    catch (...) { return 0; }
}

double PostgresStatement::columnDouble(int index) const {
    try { return std::stod(columnText(index)); }
    catch (...) { return 0.0; }
}

bool PostgresStatement::columnIsNull(int index) const {
    PGresult* result = reinterpret_cast<PGresult*>(result_);
    const int row = next_row_ - 1;
    return !result || row < 0 || index < 0 || index >= PQnfields(result) ||
        PQgetisnull(result, row, index) != 0;
}

bool postgresSqlStateIsUniqueViolation(const PostgresConnection& connection) noexcept {
    return connection.sqlState() == "23505";
}

}  // namespace yolo11_server
