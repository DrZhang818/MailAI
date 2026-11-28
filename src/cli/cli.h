#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "cli_login.h"
#include "cli_terminal.h"
#include "mailai/services.h"

namespace mailai::cli {

// args 不包含程序名
int run_cli(const std::vector<std::string>& args,
    const MailServices& services,
    std::ostream& out,
    std::ostream& err,
    const TerminalOptions& terminal = {},
    const LoginInput& login_input = {});

}  // namespace mailai::cli
