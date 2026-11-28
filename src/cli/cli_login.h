#pragma once

#include <functional>
#include <iosfwd>
#include <string>

#include "mailai/services.h"

namespace mailai::cli {

struct LoginInput {
    std::function<bool(std::string&)> read_line;
    std::function<bool(std::string&)> read_password;
};

LoginInput terminal_login_input();

int execute_login(
    const MailServices& services, std::ostream& out, std::ostream& err, const LoginInput& input);
int execute_logout(const MailServices& services, std::ostream& out, std::ostream& err);

}  // namespace mailai::cli
