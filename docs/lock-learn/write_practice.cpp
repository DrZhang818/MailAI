#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>

ssize_t limited_write(int fd, const char* data, size_t count) {
    return ::write(fd, data, std::min(count, size_t{3}));
}

int main() {
    std::string data = "ABCDEFGHIJ";

    int fd = ::open(
        "write-demo.txt",
        O_WRONLY | O_CREAT | O_TRUNC,
        0600
    );

    if (fd < 0) {
        std::perror("open");
        return 1;
    }

    size_t done = 0;

    while (done < data.size()) {
        ssize_t n = limited_write(
            fd,
            data.data() + done,
            data.size() - done
        );

        if (n <= 0) {
            std::cerr << "本次未能成功写入\n";
            ::close(fd);
            return 1;
        }

        done += n;

        std::cout << "本次写入 " << n << " 字节, 累计完成 " << done << "\n";
    }

    ::close(fd);
}