#pragma once

#include "mailai/model.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace mailai::cli {

// args 不包含程序名
int run_cli(
    const std::vector<std::string>& args,
    const std::vector<Account>& accounts,
    const std::vector<Mail>& mails,
    std::ostream& out,
    std::ostream& err
);

} // namespace mailai::cli
