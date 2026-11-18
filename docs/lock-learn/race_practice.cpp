#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>

int main(int argc, char** argv) {
    const char* name = argc > 1 ? argv[1] : "P";

    int fd = ::open("counter-demo.txt", O_RDWR);

    if (fd < 0) {
        std::perror("open");
        return 1;
    }

    char ch{};

    if (::read(fd, &ch, 1) != 1) {
        std::cerr << "未能读到一个计数字符\n";
        ::close(fd);
        return 1;
    }

    int old = ch - '0';

    std::cout << name << " 读到 " << old
              << ", 按回车后才写回"
              << std::endl;

    std::cin.get();

    int updated = old + 1;
    char output = static_cast<char>('0' + updated);

    if (::lseek(fd, 0, SEEK_SET) < 0) {
        std::perror("lseek");
        ::close(fd);
        return 1;
    }

    if (::write(fd, &output, 1) != 1) {
        std::cerr << "未能写回一个计数字符\n";
        ::close(fd);
        return 1;
    }

    if (::close(fd) < 0) {
        std::perror("close");
        return 1;
    }

    std::cout << name << " 写回 " << updated << std::endl;
}