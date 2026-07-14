#pragma once

// =========================
// Windows 防止 min/max 宏污染
// 必须放在 windows.h 前面
// =========================
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

// =========================
// C++ 标准库
// =========================
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// =========================
// OpenCV
// =========================
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>

// =========================
// 平台相关头文件
// =========================
#ifdef _WIN32
#include <windows.h>

// 双保险：防止 windows.h 里的 min/max 宏污染 std::min/std::max 或 OpenCV
#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

#else
#include <dirent.h>
#endif

static inline cv::Mat preprocess_img(cv::Mat& img, int input_w, int input_h) {
    int w, h, x, y;

    float r_w = input_w / (img.cols * 1.0f);
    float r_h = input_h / (img.rows * 1.0f);

    if (r_h > r_w) {
        w = input_w;
        h = static_cast<int>(r_w * img.rows);
        x = 0;
        y = (input_h - h) / 2;
    } else {
        w = static_cast<int>(r_h * img.cols);
        h = input_h;
        x = (input_w - w) / 2;
        y = 0;
    }

    cv::Mat re(h, w, CV_8UC3);
    cv::resize(img, re, re.size(), 0, 0, cv::INTER_LINEAR);

    cv::Mat out(input_h, input_w, CV_8UC3, cv::Scalar(128, 128, 128));
    re.copyTo(out(cv::Rect(x, y, re.cols, re.rows)));

    return out;
}

static inline int read_files_in_dir(const char* p_dir_name, std::vector<std::string>& file_names) {
#ifdef _WIN32
    std::string search_path = std::string(p_dir_name) + "\\*";

    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_path.c_str(), &find_data);

    if (hFind == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open directory: " << p_dir_name << std::endl;
        return -1;
    }

    do {
        std::string file_name = find_data.cFileName;

        if (file_name == "." || file_name == "..") {
            continue;
        }

        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            file_names.push_back(file_name);
        }

    } while (FindNextFileA(hFind, &find_data));

    FindClose(hFind);
    return 0;

#else
    DIR* p_dir = opendir(p_dir_name);

    if (p_dir == nullptr) {
        std::cerr << "Failed to open directory: " << p_dir_name << std::endl;
        return -1;
    }

    struct dirent* p_file = nullptr;

    while ((p_file = readdir(p_dir)) != nullptr) {
        if (std::strcmp(p_file->d_name, ".") != 0 && std::strcmp(p_file->d_name, "..") != 0) {
            std::string cur_file_name(p_file->d_name);
            file_names.push_back(cur_file_name);
        }
    }

    closedir(p_dir);
    return 0;
#endif
}

// Function to trim leading and trailing whitespace from a string
static inline std::string trim_leading_whitespace(const std::string& str) {
    size_t first = str.find_first_not_of(' ');

    if (std::string::npos == first) {
        return str;
    }

    size_t last = str.find_last_not_of(' ');
    return str.substr(first, last - first + 1);
}

// Src: https://stackoverflow.com/questions/16605967
static inline std::string to_string_with_precision(const float a_value, const int n = 2) {
    std::ostringstream out;
    out.precision(n);
    out << std::fixed << a_value;
    return out.str();
}

static inline int read_labels(const std::string labels_filename, std::unordered_map<int, std::string>& labels_map) {
    std::ifstream file(labels_filename);

    if (!file.is_open()) {
        std::cerr << "Failed to open labels file: " << labels_filename << std::endl;
        return -1;
    }

    std::string line;
    int index = 0;

    while (std::getline(file, line)) {
        line = trim_leading_whitespace(line);
        labels_map[index] = line;
        index++;
    }

    file.close();

    return 0;
}