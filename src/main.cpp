#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "cli/cli.h"
#include "mailai/error_message.h"

int main(int argc, char* argv[]) {
    try {
        std::vector<std::string> args;

        for (int i = 1; i < argc; i++) {
            args.emplace_back(argv[i]);
        }

        const auto services = mailai::make_runtime_services(mailai::read_runtime_environment());
        return mailai::cli::run_cli(args,
            services,
            std::cout,
            std::cerr,
            mailai::cli::detect_terminal_options(),
            mailai::cli::terminal_login_input());
    } catch (const std::exception&) {
        std::cerr << mailai::error_message::cli::execution_failed << '\n';
        return 1;
    }
}
