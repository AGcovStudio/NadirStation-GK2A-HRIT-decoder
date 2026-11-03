// qtpath.h - QString -> std::filesystem::path (wide on Windows, so folders with Chinese names work).
#pragma once
#include <QString>
#include <filesystem>

inline std::filesystem::path toFsPath(const QString& s) {
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toStdString());
#endif
}
