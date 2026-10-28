#pragma once

#include "mailai/model.h"

#include <string> 
#include <string_view>
#include <vector>

namespace mailai {

struct ParsedMail {
    std::string sender;
    std::string subject;
    std::string body;
    std::string message_id;
    std::string in_reply_to;
    std::string references;
    std::vector<AttachmentInfo> attachments;
};

Result<ParsedMail> parse_message(std::string_view raw);

} // namespace mailai