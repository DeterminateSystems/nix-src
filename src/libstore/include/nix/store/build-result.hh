#pragma once
///@file

#include "nix/store/realisation.hh"
#include "nix/store/derived-path.hh"

#include <string>
#include <chrono>
#include <optional>

#include <nlohmann/json_fwd.hpp>

namespace nix {

struct BuildResult
{
    /**
     * @note This is directly used in the nix-store --serve protocol.
     * That means we need to worry about compatibility across versions.
     * Therefore, don't remove status codes, and only add new status
     * codes at the end of the list.
     */
    enum Status {
        Built = 0,
        Substituted,
        AlreadyValid,
        PermanentFailure,
        InputRejected,
        OutputRejected,
        /// possibly transient
        TransientFailure,
        /// no longer used
        CachedFailure,
        TimedOut,
        MiscFailure,
        DependencyFailed,
        LogLimitExceeded,
        NotDeterministic,
        ResolvesToAlreadyValid,
        NoSubstituters,
    } status = MiscFailure;

    /**
     * Information about the error if the build failed.
     *
     * @todo This should be an entire ErrorInfo object, not just a
     * string, for richer information.
     */
    std::string errorMsg;

    static std::string_view statusToString(Status status)
    {
        switch (status) {
        case Built:
            return "Built";
        case Substituted:
            return "Substituted";
        case AlreadyValid:
            return "AlreadyValid";
        case PermanentFailure:
            return "PermanentFailure";
        case InputRejected:
            return "InputRejected";
        case OutputRejected:
            return "OutputRejected";
        case TransientFailure:
            return "TransientFailure";
        case CachedFailure:
            return "CachedFailure";
        case TimedOut:
            return "TimedOut";
        case MiscFailure:
            return "MiscFailure";
        case DependencyFailed:
            return "DependencyFailed";
        case LogLimitExceeded:
            return "LogLimitExceeded";
        case NotDeterministic:
            return "NotDeterministic";
        case ResolvesToAlreadyValid:
            return "ResolvesToAlreadyValid";
        case NoSubstituters:
            return "NoSubstituters";
        default:
            return "Unknown";
        };
    }

    std::string toString() const
    {
        return std::string(statusToString(status)) + ((errorMsg == "") ? "" : " : " + errorMsg);
    }

    /**
     * How many times this build was performed.
     */
    unsigned int timesBuilt = 0;

    /**
     * If timesBuilt > 1, whether some builds did not produce the same
     * result. (Note that 'isNonDeterministic = false' does not mean
     * the build is deterministic, just that we don't have evidence of
     * non-determinism.)
     */
    bool isNonDeterministic = false;

    /**
     * For derivations, a mapping from the names of the wanted outputs
     * to actual paths.
     */
    SingleDrvOutputs builtOutputs;

    /**
     * The start/stop times of the build (or one of the rounds, if it
     * was repeated).
     */
    time_t startTime = 0, stopTime = 0;

    /**
     * User and system CPU time the build took.
     */
    std::optional<std::chrono::microseconds> cpuUser, cpuSystem;

    bool operator==(const BuildResult &) const noexcept;
    std::strong_ordering operator<=>(const BuildResult &) const noexcept;

    bool success()
    {
        return status == Built || status == Substituted || status == AlreadyValid || status == ResolvesToAlreadyValid;
    }

    void rethrow()
    {
        throw Error("%s", errorMsg);
    }
};

/**
 * A `BuildResult` together with its "primary key".
 */
struct KeyedBuildResult : BuildResult
{
    /**
     * The derivation we built or the store path we substituted.
     */
    DerivedPath path;

    // Hack to work around a gcc "may be used uninitialized" warning.
    KeyedBuildResult(BuildResult res, DerivedPath path)
        : BuildResult(std::move(res))
        , path(std::move(path))
    {
    }
};

void to_json(nlohmann::json & json, const BuildResult & buildResult);
void to_json(nlohmann::json & json, const KeyedBuildResult & buildResult);

} // namespace nix
