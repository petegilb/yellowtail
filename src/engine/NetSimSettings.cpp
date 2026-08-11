//
// Created by Peter Gilbert on 8/11/26.
//

#include "NetSimSettings.h"

#include <cstdlib>

namespace ytail {
    namespace {
        template <typename T>
        void appendArg(std::vector<std::string>& args, const char* name, const T value) {
            args.emplace_back(name);
            args.push_back(std::to_string(value));
        }
    }

    std::vector<std::string> netSimToArgs(const NetSimSettings& settings) {
        std::vector<std::string> args;
        if (!settings.isActive()) return args;

        appendArg(args, "--net-lag", settings.lagMs);
        appendArg(args, "--net-jitter", settings.jitterMs);
        appendArg(args, "--net-jitter-pct", settings.jitterPct);
        appendArg(args, "--net-loss", settings.lossPct);
        appendArg(args, "--net-reorder", settings.reorderPct);
        appendArg(args, "--net-reorder-time", settings.reorderTimeMs);
        return args;
    }

    NetSimSettings netSimFromArgs(const int argc, char* argv[]) {
        NetSimSettings settings;
        for (int i = 1; i + 1 < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--net-lag") {
                settings.lagMs = std::atoi(argv[++i]);
            } else if (arg == "--net-jitter") {
                settings.jitterMs = std::strtof(argv[++i], nullptr);
            } else if (arg == "--net-jitter-pct") {
                settings.jitterPct = std::strtof(argv[++i], nullptr);
            } else if (arg == "--net-loss") {
                settings.lossPct = std::strtof(argv[++i], nullptr);
            } else if (arg == "--net-reorder") {
                settings.reorderPct = std::strtof(argv[++i], nullptr);
            } else if (arg == "--net-reorder-time") {
                settings.reorderTimeMs = std::atoi(argv[++i]);
            }
        }
        return settings;
    }
} // ytail
