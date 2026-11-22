// OpenScarlett - MIT License
// Standalone CLI runner for GFX10.3 (RDNA2) 779-opcode ISA coverage & 3,120-shader corpus benchmark.

#include <cstring>
#include <string>
#include <vector>

#include "rdna2_coverage_tool.h"

int main(int argc, char** argv) {
    bool has_mode_or_help = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--full") == 0 ||
            std::strcmp(argv[i], "--fast") == 0 ||
            std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            has_mode_or_help = true;
            break;
        }
    }

    std::string subcmd = "rdna2-bench";
    std::string full_flag = "--full";
    std::vector<char*> fwd_argv;
    fwd_argv.reserve(static_cast<size_t>(argc) + 3u);
    fwd_argv.push_back(argv[0]);
    fwd_argv.push_back(subcmd.data());
    if (!has_mode_or_help) {
        fwd_argv.push_back(full_flag.data());
    }
    for (int i = 1; i < argc; ++i) {
        fwd_argv.push_back(argv[i]);
    }
    return openscarlett::RunRdna2BenchCli(static_cast<int>(fwd_argv.size()), fwd_argv.data());
}
