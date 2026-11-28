#pragma once

#include <iosfwd>
#include <vector>

#include "cli_terminal.h"
#include "mailai/services.h"

namespace mailai::cli {

void print_help(std::ostream& out);
void print_error(std::ostream& err, const Error& error);
void print_inbox(std::ostream& out,
    DataSource source,
    const std::vector<InboxItem>& items,
    const TerminalOptions& terminal);
void print_mail(
    std::ostream& out, DataSource source, const Mail& mail, const TerminalOptions& terminal);
void print_sync_summary(std::ostream& out, std::ostream& err, const SyncSummary& summary);

}  // namespace mailai::cli
