#include "nix/store/build-result.hh"

#include <nlohmann/json.hpp>

namespace nix {

bool BuildResult::operator==(const BuildResult &) const noexcept = default;
std::strong_ordering BuildResult::operator<=>(const BuildResult &) const noexcept = default;

bool BuildResult::Success::operator==(const BuildResult::Success &) const noexcept = default;
std::strong_ordering BuildResult::Success::operator<=>(const BuildResult::Success &) const noexcept = default;

bool BuildResult::Failure::operator==(const BuildResult::Failure &) const noexcept = default;
std::strong_ordering BuildResult::Failure::operator<=>(const BuildResult::Failure &) const noexcept = default;

static std::string_view statusToString(BuildResult::Success::Status status)
{
    switch (status) {
    case BuildResult::Success::Built:
        return "Built";
    case BuildResult::Success::Substituted:
        return "Substituted";
    case BuildResult::Success::AlreadyValid:
        return "AlreadyValid";
    case BuildResult::Success::ResolvesToAlreadyValid:
        return "ResolvesToAlreadyValid";
    default:
        unreachable();
    }
}

static std::string_view statusToString(BuildResult::Failure::Status status)
{
    switch (status) {
    case BuildResult::Failure::PermanentFailure:
        return "PermanentFailure";
    case BuildResult::Failure::InputRejected:
        return "InputRejected";
    case BuildResult::Failure::OutputRejected:
        return "OutputRejected";
    case BuildResult::Failure::TransientFailure:
        return "TransientFailure";
    case BuildResult::Failure::CachedFailure:
        return "CachedFailure";
    case BuildResult::Failure::TimedOut:
        return "TimedOut";
    case BuildResult::Failure::MiscFailure:
        return "MiscFailure";
    case BuildResult::Failure::DependencyFailed:
        return "DependencyFailed";
    case BuildResult::Failure::LogLimitExceeded:
        return "LogLimitExceeded";
    case BuildResult::Failure::NotDeterministic:
        return "NotDeterministic";
    case BuildResult::Failure::NoSubstituters:
        return "NoSubstituters";
    case BuildResult::Failure::HashMismatch:
        return "HashMismatch";
    default:
        unreachable();
    }
}

void to_json(nlohmann::json & json, const BuildResult & buildResult)
{
    json = nlohmann::json::object();
    // FIXME: change this to have `success` and `failure` objects.
    if (auto success = buildResult.tryGetSuccess()) {
        json["status"] = statusToString(success->status);
    } else if (auto failure = buildResult.tryGetFailure()) {
        json["status"] = statusToString(failure->status);
        if (failure->errorMsg != "")
            json["errorMsg"] = failure->errorMsg;
        if (failure->isNonDeterministic)
            json["isNonDeterministic"] = failure->isNonDeterministic;
    }
    if (buildResult.timesBuilt)
        json["timesBuilt"] = buildResult.timesBuilt;
    if (buildResult.startTime)
        json["startTime"] = buildResult.startTime;
    if (buildResult.stopTime)
        json["stopTime"] = buildResult.stopTime;
}

void to_json(nlohmann::json & json, const KeyedBuildResult & buildResult)
{
    to_json(json, (const BuildResult &) buildResult);
    auto path = nlohmann::json::object();
    std::visit(
        overloaded{
            [&](const DerivedPathOpaque & opaque) { path["opaque"] = opaque.path.to_string(); },
            [&](const DerivedPathBuilt & drv) {
                path["drvPath"] = drv.drvPath->getBaseStorePath().to_string();
                path["outputs"] = drv.outputs;
                auto outputs = nlohmann::json::object();
                if (auto success = buildResult.tryGetSuccess()) {
                    for (auto & [name, output] : success->builtOutputs)
                        outputs[name] = {
                            {"path", output.outPath.to_string()},
                            {"signatures", output.signatures},
                        };
                    json["builtOutputs"] = std::move(outputs);
                }
            },
        },
        buildResult.path.raw());
    json["path"] = std::move(path);
}

} // namespace nix
