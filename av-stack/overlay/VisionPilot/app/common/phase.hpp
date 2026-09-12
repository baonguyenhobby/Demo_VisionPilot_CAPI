#pragma once

#include <cstring>
#include <filesystem>
#include <string>

// Two-phase startup, per the execution manifest.
//
// Each Adaptive Application is deployed as TWO Processes over ONE executable:
//
//   <name>_init_process   AvPilotFG.Init     --phase=init   self-terminating
//   <name>_process        AvPilotFG.Driving  --phase=run    long-running
//
// They are separate processes, so NOTHING built in memory during init survives
// into run. Init exists to fail fast -- config parses, files open, devices are
// present -- not to warm anything up. Anything that must carry over has to go
// through a file or shared memory, and then it is state, not warm-up.
//
// No argument means run. That keeps the binaries usable outside EM, where
// nobody is passing process arguments.

namespace vpap {

enum class Phase { Init, Run };

inline Phase parse_phase(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--phase=init") == 0) return Phase::Init;
        if (std::strcmp(argv[i], "--phase=run")  == 0) return Phase::Run;
    }
    return Phase::Run;
}

inline const char* to_string(Phase p)
{
    return p == Phase::Init ? "init" : "run";
}

// Validation helpers for the init phase. Each logs the offending path itself,
// because "init failed" in an EM log with no path is a useless message.
inline bool require_file(const std::string& path, const char* what, const char* who)
{
    if (path.empty())
    {
        VP_ERROR("%s: init: %s is not configured", who, what);
        return false;
    }
    if (!std::filesystem::exists(path))
    {
        VP_ERROR("%s: init: %s not found: %s", who, what, path.c_str());
        return false;
    }
    return true;
}

inline bool require_positive(double v, const char* what, const char* who)
{
    if (!(v > 0.0))
    {
        VP_ERROR("%s: init: %s must be > 0, got %f", who, what, v);
        return false;
    }
    return true;
}

}  // namespace vpap
