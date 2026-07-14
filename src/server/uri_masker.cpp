#include "server/uri_masker.h"

#include <algorithm>
#include <cctype>

namespace yolo11_server {

    namespace {

        std::string toLower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        bool startsWith(const std::string& value, const std::string& prefix) {
            return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
        }

    }  // namespace

    bool isRtspUri(const std::string& value) {
        const std::string lower = toLower(value);
        return startsWith(lower, "rtsp://") || startsWith(lower, "rtsps://");
    }

    std::string maskRtspUri(const std::string& value) {
        if (!isRtspUri(value)) {
            return "rtsp://***";
        }

        const std::string lower = toLower(value);
        const std::string scheme = startsWith(lower, "rtsps://") ? "rtsps://" : "rtsp://";
        const std::size_t authority_begin = scheme.size();
        const std::size_t authority_end = value.find_first_of("/?#", authority_begin);
        const std::size_t end = authority_end == std::string::npos ? value.size() : authority_end;
        if (end <= authority_begin) {
            return scheme + "***";
        }

        const std::size_t at = value.rfind('@', end - 1);
        if (at == std::string::npos || at < authority_begin) {
            // A credential-free RTSP URI is safe to display, but query and
            // fragment values are removed because vendors may place tokens
            // there.
            const std::size_t safe_end = value.find_first_of("?#", authority_begin);
            return safe_end == std::string::npos ? value : value.substr(0, safe_end);
        }

        const std::size_t safe_end = value.find_first_of("?#", at + 1);
        const std::string host_and_suffix = value.substr(
            at + 1,
            safe_end == std::string::npos ? std::string::npos : safe_end - (at + 1)
        );
        if (host_and_suffix.empty()) {
            return scheme + "***";
        }
        return scheme + "***:***@" + host_and_suffix;
    }

    std::string maskStreamSource(const std::string& source_type, const std::string& value) {
        const std::string lower_type = toLower(source_type);
        if (lower_type == "rtsp" || isRtspUri(value)) {
            return maskRtspUri(value);
        }
        return value;
    }

}  // namespace yolo11_server
