#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <iostream>

int value = 10;

int main() {
    pid_t pid = ::fork();

    if (pid < 0) {
        std::perror("fork");
        return 1;
    }

    if (pid == 0) {
        value = 20;

        std::cout << "child: pid=" << ::getpid()
                  << ", parent=" << ::getppid()
                  << ", value=" << value
                  << ", address=" << &value
                  << std::endl;
        
        return 0;
    }

    pid_t waited;

    do {
        waited = ::waitpid(pid, nullptr, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        std::perror("waitpid");
        return 1;
    }

    std::cout << "parent: pid=" << ::getpid()
              << ", child=" << pid
              << ", value=" << value
              << ", address=" << &value
              << std::endl;
}