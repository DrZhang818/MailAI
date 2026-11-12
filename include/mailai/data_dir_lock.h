#pragma once

#include <chrono>
#include <filesystem>

#include "mailai/model.h"

namespace mailai {

class DataDirLock {
public:
    static Result<DataDirLock> acquire(
        const std::filesystem::path& data_dir,
        std::chrono::milliseconds wait = std::chrono::seconds{5}
    );

    ~DataDirLock();

    DataDirLock(const DataDirLock&) = delete;
    DataDirLock& operator=(const DataDirLock&) = delete;

    DataDirLock(DataDirLock&& other) noexcept;
    DataDirLock& operator=(DataDirLock&& other) noexcept;

private:
    explicit DataDirLock(int fd) noexcept : fd_(fd) {}

    int fd_{-1};
};

} // namespace mailai