#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

namespace yolo11_server {

// Avoid passing std::string across the yaml-cpp DLL boundary. The repository's
// MSVC runtime can be newer than the prebuilt vcpkg yaml-cpp runtime; its
// LoadFile(std::string) ABI then interprets the filename object incorrectly.
// File I/O stays in this module and yaml-cpp receives only a stable C buffer.
inline YAML::Node loadYamlFileAbiSafe(const std::string& yaml_path) {
    if (yaml_path.empty()) throw std::runtime_error("YAML path is empty");
    const auto path = std::filesystem::u8path(yaml_path);
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open YAML file");
    std::string bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!input.eof() && input.fail()) throw std::runtime_error("cannot read YAML file");
    if (bytes.find('\0') != std::string::npos) {
        throw std::runtime_error("YAML file contains a NUL byte");
    }
    return YAML::Load(bytes.c_str());
}

}  // namespace yolo11_server
