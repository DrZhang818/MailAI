#include "cli.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    try {
        const std::vector<mailai::Account> accounts{
            {"school", "学校邮箱", "student@school.example"},
            {"qq", "QQ邮箱", "student@qq.example"}
        };

        const std::vector<mailai::Mail> mails{
            {44, "school", "library@school.example", "图书馆通知", "请按时归还图书", 100},
            {43, "qq", "recruiter@example.com", "实习通知", "请确认面试时间", 200},
            {42, "school", "course@school.example", "课程通知", "请查看课程安排", 200}
        };

        std::vector<std::string> args;

        for (int i = 1; i < argc; i++) {
            args.emplace_back(argv[i]);
        }

        return mailai::cli::run_cli(args, accounts, mails, std::cout, std::cerr);
    } catch (const std::exception& error) {
        std::cerr << "程序执行失败: " << error.what() << "\n";
        return 1;
    }
}
