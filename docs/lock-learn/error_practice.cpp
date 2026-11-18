#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <iostream>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "write-damo.txt";

    int fd = ::open(path, O_RDONLY);

    if (fd < 0) {
        const int error = errno;

        std::perror("open");

        std::cout << "保存的错误码: " << error << "\n";
        std::cout << "是否为 ENOENT: " << (error == ENOENT) << "\n";
        return 1;
    }

    ssize_t n = ::write(fd, "X", 1);

    if (n < 0) {
        const int error = errno;

        std::perror("write");

        ::close(fd);

        std::cout << "保存的错误码: " << error << "\n";
        std::cout << "是否为 EBADF: " << (error == EBADF) << "\n";
        return 1;
    }

    ::close(fd);
}