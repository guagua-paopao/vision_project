#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <openssl/sha.h>

#include "server/callback_delivery_worker.h"

namespace {

using namespace yolo11_server;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct WinsockScope {
    WinsockScope() {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockScope() { if (ready) WSACleanup(); }
    bool ready = false;
};

std::size_t contentLength(const std::string& headers) {
    const std::string key = "Content-Length:";
    const auto position = headers.find(key);
    if (position == std::string::npos) return 0;
    const auto value_start = headers.find_first_not_of(" ", position + key.size());
    const auto value_end = headers.find("\r\n", value_start);
    return static_cast<std::size_t>(std::stoull(
        headers.substr(value_start, value_end - value_start)));
}

std::string sha256Hex(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char byte : digest) {
        output << std::setw(2) << static_cast<int>(byte);
    }
    return output.str();
}

}  // namespace

int main() {
    WinsockScope winsock;
    require(winsock.ready, "Winsock must initialize");

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "loopback listener socket must open");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(
            listener,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0,
        "loopback listener must bind");
    require(listen(listener, 1) == 0, "loopback listener must listen");
    int address_size = sizeof(address);
    require(getsockname(
            listener,
            reinterpret_cast<sockaddr*>(&address),
            &address_size) == 0,
        "loopback port must resolve");
    const int port = ntohs(address.sin_port);

    std::string captured;
    const std::string response_body(256, 'r');
    std::atomic<bool> server_ok{ false };
    std::thread server([&]() {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval wait{ 5, 0 };
        if (select(0, &readable, nullptr, nullptr, &wait) <= 0) return;
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        DWORD timeout_ms = 5000;
        setsockopt(
            client,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout_ms),
            sizeof(timeout_ms));
        char buffer[2048];
        std::size_t expected = 0;
        while (true) {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            captured.append(buffer, static_cast<std::size_t>(count));
            const auto header_end = captured.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                if (expected == 0) {
                    expected = header_end + 4 +
                        contentLength(captured.substr(0, header_end + 2));
                }
                if (captured.size() >= expected) break;
            }
        }
        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: " + std::to_string(response_body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + response_body;
        server_ok.store(send(
            client,
            response.data(),
            static_cast<int>(response.size()),
            0) == static_cast<int>(response.size()));
        shutdown(client, SD_BOTH);
        closesocket(client);
    });

    auto transport = createProductionCallbackHttpTransport();
    require(transport != nullptr, "production callback transport must construct");
    CallbackHttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(port) +
        "/callback/events?source=vision";
    request.body = R"({"event_id":"evt_loopback","value":1})";
    request.allow_insecure_http = true;
    request.timeout_ms = 5000;
    request.response_body_limit_bytes = 128;
    request.headers = {
        { "Idempotency-Key", "evt_loopback" },
        { "X-Event-Id", "evt_loopback" },
        { "X-Signature", "0123456789abcdef" },
        { "X-Signature-Version", "1" },
        { "X-Timestamp", "1784800000000" }
    };
    const auto response = transport->post(request);
    server.join();
    closesocket(listener);
    require(response.transport_ok && response.status_code == 200 &&
            response.body.size() == 128 && response.body_truncated &&
            response.body_hash == sha256Hex(response_body) &&
            server_ok.load(),
        "WinHTTP must deliver to the real loopback HTTP receiver");
    require(captured.find(
                "POST /callback/events?source=vision HTTP/1.1\r\n") == 0 &&
            captured.find("Content-Type: application/json\r\n") != std::string::npos &&
            captured.find("Idempotency-Key: evt_loopback\r\n") != std::string::npos &&
            captured.find("X-Event-Id: evt_loopback\r\n") != std::string::npos &&
            captured.find("X-Signature-Version: 1\r\n") != std::string::npos &&
            captured.substr(captured.find("\r\n\r\n") + 4) == request.body,
        "real HTTP request must preserve method, path, required headers, and exact body bytes");

    request.allow_insecure_http = false;
    request.url = "http://127.0.0.1:1/forbidden";
    const auto insecure = transport->post(request);
    require(!insecure.transport_ok &&
            insecure.error_code == "CALLBACK_INSECURE_HTTP_FORBIDDEN",
        "plain HTTP must fail closed unless the deployment profile explicitly allows it");

    request.allow_insecure_http = true;
    request.url = "http://user:password@127.0.0.1:1/forbidden";
    const auto credentials = transport->post(request);
    require(!credentials.transport_ok &&
            credentials.error_code == "CALLBACK_URL_INVALID",
        "callback URLs containing userinfo credentials must be rejected");

    std::cout << "WinHTTP callback transport loopback tests passed\n";
    return 0;
}
