#include "core/specimen_browser.hpp"

namespace artminer::core {

std::string ParameterLocks::serialize_canonical() const {
    std::string result;
    const auto append = [&result](const char kind, const LockKey& key) {
        if (!result.empty()) {
            result.push_back(';');
        }
        result.push_back(kind);
        result.push_back('/');
        result += key.first;
        result.push_back('/');
        result += key.second;
    };
    for (const auto& key : parameters_) {
        append('p', key);
    }
    for (const auto& key : groups_) {
        append('g', key);
    }
    return result;
}

}  // namespace artminer::core
