#include "quarry/diversity.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "core/graph.hpp"
#include "core/hash.hpp"

namespace artminer::quarry {
namespace {

[[nodiscard]] DiversityError error(const DiversityErrorCode code, std::string message) {
    return DiversityError{code, std::move(message)};
}

[[nodiscard]] const MetricValue* find_metric(const MetricVector& metrics, const std::string_view name) noexcept {
    const auto found = std::find_if(metrics.begin(), metrics.end(), [name](const MetricValue& metric) {
        return metric.name == name;
    });
    return found == metrics.end() ? nullptr : &*found;
}

[[nodiscard]] const NormalizedMetric* find_normalized(
    const CandidateFeatures& candidate,
    const std::string_view name) noexcept {
    const auto found = std::find_if(candidate.metrics.begin(), candidate.metrics.end(), [name](const NormalizedMetric& metric) {
        return metric.name == name;
    });
    return found == candidate.metrics.end() ? nullptr : &*found;
}

[[nodiscard]] double normalized_parameter_value(
    const core::ParameterValue& value,
    const core::ParameterSpec& spec) noexcept {
    switch (spec.kind) {
    case core::ParameterKind::integer: {
        const auto* integer = std::get_if<core::i64>(&value);
        if (integer == nullptr || !spec.domain.integer_min.has_value() || !spec.domain.integer_max.has_value() ||
            *spec.domain.integer_max <= *spec.domain.integer_min) {
            return 0.0;
        }
        return static_cast<double>(*integer - *spec.domain.integer_min) /
               static_cast<double>(*spec.domain.integer_max - *spec.domain.integer_min);
    }
    case core::ParameterKind::real: {
        const auto* real = std::get_if<double>(&value);
        if (real == nullptr || !spec.domain.real_min.has_value() || !spec.domain.real_max.has_value() ||
            !(*spec.domain.real_max > *spec.domain.real_min)) {
            return 0.0;
        }
        return (*real - *spec.domain.real_min) / (*spec.domain.real_max - *spec.domain.real_min);
    }
    case core::ParameterKind::boolean: {
        const auto* boolean = std::get_if<bool>(&value);
        return boolean != nullptr && *boolean ? 1.0 : 0.0;
    }
    case core::ParameterKind::enumeration: {
        const auto* enumeration = std::get_if<std::string>(&value);
        if (enumeration == nullptr || spec.domain.enum_values.size() <= 1U) {
            return 0.0;
        }
        const auto found = std::find(spec.domain.enum_values.begin(), spec.domain.enum_values.end(), *enumeration);
        if (found == spec.domain.enum_values.end()) {
            return 0.0;
        }
        const std::size_t index = static_cast<std::size_t>(std::distance(spec.domain.enum_values.begin(), found));
        return static_cast<double>(index) / static_cast<double>(spec.domain.enum_values.size() - 1U);
    }
    }
    return 0.0;
}

[[nodiscard]] core::Result<std::vector<double>, DiversityError> parameter_vector(const core::Recipe& recipe) {
    std::vector<const core::NodeInstance*> ordered_nodes;
    ordered_nodes.reserve(recipe.nodes.size());
    for (const auto& node : recipe.nodes) {
        ordered_nodes.push_back(&node);
    }
    std::sort(ordered_nodes.begin(), ordered_nodes.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });

    std::vector<double> output;
    for (const core::NodeInstance* node : ordered_nodes) {
        const core::NodeMetadata* metadata = core::builtin_node_registry().find(node->type_id);
        if (metadata == nullptr) {
            return core::Result<std::vector<double>, DiversityError>::failure(error(
                DiversityErrorCode::incompatible_recipe,
                "candidate recipe contains an unknown node while building parameter distance vector"));
        }
        std::vector<const core::ParameterSpec*> specs;
        specs.reserve(metadata->parameters.size());
        for (const auto& spec : metadata->parameters) {
            if (spec.mutation.mutable_parameter) {
                specs.push_back(&spec);
            }
        }
        std::sort(specs.begin(), specs.end(), [](const auto* left, const auto* right) {
            return left->name < right->name;
        });
        for (const core::ParameterSpec* spec : specs) {
            const auto assignment = std::find_if(node->parameters.begin(), node->parameters.end(), [spec](const auto& value) {
                return value.name == spec->name;
            });
            if (assignment == node->parameters.end()) {
                return core::Result<std::vector<double>, DiversityError>::failure(error(
                    DiversityErrorCode::incompatible_recipe,
                    "candidate recipe is missing a parameter required for parameter distance"));
            }
            output.push_back(std::clamp(normalized_parameter_value(assignment->value, *spec), 0.0, 1.0));
        }
    }
    return core::Result<std::vector<double>, DiversityError>::success(std::move(output));
}

[[nodiscard]] std::vector<const CandidateFeatures*> sorted_candidates(
    const std::vector<CandidateFeatures>& candidates) {
    std::vector<const CandidateFeatures*> sorted;
    sorted.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        sorted.push_back(&candidate);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
        return left->index < right->index;
    });
    return sorted;
}

[[nodiscard]] const CandidateFeatures* by_index(
    const std::vector<CandidateFeatures>& candidates,
    const core::u64 index) noexcept {
    const auto found = std::find_if(candidates.begin(), candidates.end(), [index](const CandidateFeatures& candidate) {
        return candidate.index == index;
    });
    return found == candidates.end() ? nullptr : &*found;
}

[[nodiscard]] double distance_to_centroid(
    const CandidateFeatures& candidate,
    const std::vector<double>& centroid,
    const NormalizationModel& normalization,
    std::vector<MetricContribution>* contributions = nullptr) noexcept {
    double sum = 0.0;
    double total_weight = 0.0;
    if (contributions != nullptr) {
        contributions->clear();
    }
    for (std::size_t dimension = 0U; dimension < normalization.dimensions.size(); ++dimension) {
        const auto& model = normalization.dimensions[dimension];
        if (model.weight <= 0.0 || model.degenerate || dimension >= centroid.size()) {
            continue;
        }
        const NormalizedMetric* metric = find_normalized(candidate, model.name);
        if (metric == nullptr || !metric->available) {
            continue;
        }
        const double delta = metric->normalized - centroid[dimension];
        const double term = model.weight * delta * delta;
        sum += term;
        total_weight += model.weight;
        if (contributions != nullptr) {
            contributions->push_back(MetricContribution{model.name, term});
        }
    }
    if (!(total_weight > 0.0)) {
        return 0.0;
    }
    if (contributions != nullptr) {
        for (auto& contribution : *contributions) {
            contribution.contribution /= total_weight;
        }
        std::sort(contributions->begin(), contributions->end(), [](const auto& left, const auto& right) {
            if (left.contribution != right.contribution) {
                return left.contribution > right.contribution;
            }
            return left.name < right.name;
        });
    }
    return std::sqrt(sum / total_weight);
}

[[nodiscard]] std::vector<double> centroid_for(
    const std::vector<const CandidateFeatures*>& members,
    const NormalizationModel& normalization) {
    std::vector<double> centroid(normalization.dimensions.size(), 0.0);
    std::vector<std::size_t> counts(normalization.dimensions.size(), 0U);
    for (const CandidateFeatures* member : members) {
        for (std::size_t dimension = 0U; dimension < normalization.dimensions.size(); ++dimension) {
            const NormalizedMetric* metric = find_normalized(*member, normalization.dimensions[dimension].name);
            if (metric != nullptr && metric->available) {
                centroid[dimension] += metric->normalized;
                ++counts[dimension];
            }
        }
    }
    for (std::size_t dimension = 0U; dimension < centroid.size(); ++dimension) {
        if (counts[dimension] != 0U) {
            centroid[dimension] /= static_cast<double>(counts[dimension]);
        }
    }
    return centroid;
}

void set_metadata(core::Recipe& recipe, std::string key, std::string value) {
    auto found = std::find_if(recipe.metadata.begin(), recipe.metadata.end(), [&key](const core::RecipeMetadata& metadata) {
        return metadata.key == key;
    });
    if (found == recipe.metadata.end()) {
        recipe.metadata.push_back(core::RecipeMetadata{std::move(key), std::move(value)});
    } else {
        found->value = std::move(value);
    }
}

}  // namespace

core::Result<NormalizationModel, DiversityError> build_normalization_model(
    const std::vector<CandidateResult>& results,
    const std::vector<MetricWeight>& weights) {
    if (results.empty()) {
        return core::Result<NormalizationModel, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings, "normalization requires at least one Quarry result"));
    }

    std::vector<MetricWeight> effective = weights;
    if (effective.empty()) {
        effective.reserve(results.front().metrics.size());
        for (const MetricValue& metric : results.front().metrics) {
            effective.push_back(MetricWeight{metric.name, 1.0});
        }
    }
    if (effective.empty()) {
        return core::Result<NormalizationModel, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings, "normalization requires at least one selected metric"));
    }

    std::set<std::string> names;
    bool any_positive_weight = false;
    NormalizationModel model;
    model.dimensions.reserve(effective.size());
    for (const MetricWeight& requested : effective) {
        if (requested.name.empty() || !std::isfinite(requested.weight) || requested.weight < 0.0 ||
            !names.insert(requested.name).second) {
            return core::Result<NormalizationModel, DiversityError>::failure(error(
                DiversityErrorCode::invalid_settings,
                "metric weights require unique non-empty names and finite non-negative weights"));
        }
        any_positive_weight = any_positive_weight || requested.weight > 0.0;
        double minimum = (std::numeric_limits<double>::infinity)();
        double maximum = -(std::numeric_limits<double>::infinity)();
        bool found = false;
        for (const CandidateResult& result : results) {
            const MetricValue* value = find_metric(result.metrics, requested.name);
            if (value == nullptr || !std::isfinite(value->value)) {
                continue;
            }
            minimum = (std::min)(minimum, value->value);
            maximum = (std::max)(maximum, value->value);
            found = true;
        }
        if (!found) {
            return core::Result<NormalizationModel, DiversityError>::failure(error(
                DiversityErrorCode::missing_metric,
                "selected metric '" + requested.name + "' is absent from every Quarry result"));
        }
        model.dimensions.push_back(NormalizationDimension{
            requested.name,
            minimum,
            maximum,
            requested.weight,
            !(maximum > minimum)});
    }
    if (!any_positive_weight) {
        return core::Result<NormalizationModel, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings, "at least one selected metric weight must be positive"));
    }
    return core::Result<NormalizationModel, DiversityError>::success(std::move(model));
}

core::Result<std::vector<CandidateFeatures>, DiversityError> build_candidate_features(
    const JobManifest& manifest,
    const std::vector<CandidateResult>& results,
    const std::filesystem::path& cache_directory,
    const NormalizationModel& normalization) {
    std::vector<CandidateFeatures> features;
    features.reserve(results.size());
    for (const CandidateResult& result : results) {
        CandidateFeatures candidate;
        candidate.index = result.index;
        candidate.candidate_id = result.candidate_id;
        candidate.recipe_fingerprint = result.recipe_fingerprint;
        candidate.raw_metrics = result.metrics;
        candidate.metrics.reserve(normalization.dimensions.size());
        for (const auto& dimension : normalization.dimensions) {
            const MetricValue* raw = find_metric(result.metrics, dimension.name);
            if (raw == nullptr || !std::isfinite(raw->value)) {
                candidate.metrics.push_back(NormalizedMetric{dimension.name, 0.0, 0.0, false, dimension.degenerate});
                continue;
            }
            const double normalized = dimension.degenerate
                ? 0.0
                : std::clamp((raw->value - dimension.minimum) / (dimension.maximum - dimension.minimum), 0.0, 1.0);
            candidate.metrics.push_back(NormalizedMetric{
                dimension.name, raw->value, normalized, true, dimension.degenerate});
        }

        auto recipe = reconstruct_candidate_recipe(manifest, result.index);
        if (recipe.is_error()) {
            return core::Result<std::vector<CandidateFeatures>, DiversityError>::failure(error(
                DiversityErrorCode::quarry_error,
                "could not reconstruct Quarry candidate #" + std::to_string(result.index) + ": " + recipe.error().message));
        }
        if (core::semantic_fingerprint(recipe.value()) != result.recipe_fingerprint) {
            return core::Result<std::vector<CandidateFeatures>, DiversityError>::failure(error(
                DiversityErrorCode::incompatible_recipe,
                "reconstructed candidate fingerprint does not match the authoritative Quarry result row"));
        }
        auto parameters = parameter_vector(recipe.value());
        if (parameters.is_error()) {
            return core::Result<std::vector<CandidateFeatures>, DiversityError>::failure(parameters.error());
        }
        candidate.parameters = std::move(parameters).value();

        auto thumbnail = read_cached_thumbnail(manifest, result.index, cache_directory);
        if (thumbnail.is_error()) {
            return core::Result<std::vector<CandidateFeatures>, DiversityError>::failure(error(
                DiversityErrorCode::quarry_error,
                "could not obtain canonical candidate thumbnail: " + thumbnail.error().message));
        }
        candidate.thumbnail_signature = make_thumbnail_signature(thumbnail.value());
        features.push_back(std::move(candidate));
    }
    std::sort(features.begin(), features.end(), [](const auto& left, const auto& right) {
        return left.index < right.index;
    });
    return core::Result<std::vector<CandidateFeatures>, DiversityError>::success(std::move(features));
}

double metric_distance(
    const CandidateFeatures& left,
    const CandidateFeatures& right,
    const NormalizationModel& normalization) noexcept {
    double sum = 0.0;
    double total_weight = 0.0;
    for (const auto& dimension : normalization.dimensions) {
        if (dimension.weight <= 0.0 || dimension.degenerate) {
            continue;
        }
        const NormalizedMetric* a = find_normalized(left, dimension.name);
        const NormalizedMetric* b = find_normalized(right, dimension.name);
        if (a == nullptr || b == nullptr || !a->available || !b->available) {
            continue;
        }
        const double delta = a->normalized - b->normalized;
        sum += dimension.weight * delta * delta;
        total_weight += dimension.weight;
    }
    return total_weight > 0.0 ? std::sqrt(sum / total_weight) : 0.0;
}

double parameter_distance(const CandidateFeatures& left, const CandidateFeatures& right) noexcept {
    if (left.parameters.size() != right.parameters.size()) {
        return 1.0;
    }
    if (left.parameters.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t index = 0U; index < left.parameters.size(); ++index) {
        const double delta = left.parameters[index] - right.parameters[index];
        sum += delta * delta;
    }
    return std::sqrt(sum / static_cast<double>(left.parameters.size()));
}

core::Result<std::vector<DedupeGroup>, DiversityError> deduplicate_candidates(
    const std::vector<CandidateFeatures>& candidates,
    const NormalizationModel& normalization,
    const DedupeSettings& settings) {
    if (!std::isfinite(settings.metric_distance_threshold) || settings.metric_distance_threshold < 0.0 ||
        !std::isfinite(settings.image_mean_absolute_threshold) || settings.image_mean_absolute_threshold < 0.0 ||
        settings.image_mean_absolute_threshold > 1.0) {
        return core::Result<std::vector<DedupeGroup>, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings, "dedupe thresholds must be finite and non-negative; image threshold is within [0,1]"));
    }
    std::vector<DedupeGroup> groups;
    for (const CandidateFeatures* candidate : sorted_candidates(candidates)) {
        bool grouped = false;
        for (DedupeGroup& group : groups) {
            const CandidateFeatures* representative = by_index(candidates, group.representative);
            if (representative == nullptr) {
                continue;
            }
            if (metric_distance(*candidate, *representative, normalization) <= settings.metric_distance_threshold &&
                thumbnail_signature_distance(candidate->thumbnail_signature, representative->thumbnail_signature) <=
                    settings.image_mean_absolute_threshold) {
                group.members.push_back(candidate->index);
                grouped = true;
                break;
            }
        }
        if (!grouped) {
            groups.push_back(DedupeGroup{candidate->index, {candidate->index}});
        }
    }
    return core::Result<std::vector<DedupeGroup>, DiversityError>::success(std::move(groups));
}

core::Result<std::vector<Cluster>, DiversityError> cluster_candidates(
    const std::vector<CandidateFeatures>& candidates,
    const NormalizationModel& normalization,
    const std::size_t cluster_count,
    const std::vector<DedupeGroup>* dedupe) {
    if (candidates.empty() || cluster_count == 0U) {
        return core::Result<std::vector<Cluster>, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings, "clustering requires candidates and a non-zero cluster count"));
    }

    std::vector<const CandidateFeatures*> active;
    if (dedupe != nullptr) {
        active.reserve(dedupe->size());
        for (const DedupeGroup& group : *dedupe) {
            const CandidateFeatures* representative = by_index(candidates, group.representative);
            if (representative == nullptr) {
                return core::Result<std::vector<Cluster>, DiversityError>::failure(error(
                    DiversityErrorCode::candidate_missing, "dedupe representative is absent from the candidate set"));
            }
            active.push_back(representative);
        }
    } else {
        active = sorted_candidates(candidates);
    }
    if (active.empty()) {
        return core::Result<std::vector<Cluster>, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings, "clustering has no active candidates after deduplication"));
    }
    std::sort(active.begin(), active.end(), [](const auto* left, const auto* right) { return left->index < right->index; });

    const std::size_t count = (std::min)(cluster_count, active.size());
    std::vector<const CandidateFeatures*> seeds;
    seeds.reserve(count);
    seeds.push_back(active.front());
    while (seeds.size() < count) {
        const CandidateFeatures* best = nullptr;
        double best_distance = -1.0;
        for (const CandidateFeatures* candidate : active) {
            if (std::find(seeds.begin(), seeds.end(), candidate) != seeds.end()) {
                continue;
            }
            double nearest = (std::numeric_limits<double>::infinity)();
            for (const CandidateFeatures* seed : seeds) {
                nearest = (std::min)(nearest, metric_distance(*candidate, *seed, normalization));
            }
            if (nearest > best_distance || (nearest == best_distance && (best == nullptr || candidate->index < best->index))) {
                best = candidate;
                best_distance = nearest;
            }
        }
        if (best == nullptr) {
            break;
        }
        seeds.push_back(best);
    }

    std::vector<std::vector<const CandidateFeatures*>> memberships(seeds.size());
    for (const CandidateFeatures* candidate : active) {
        std::size_t best_cluster = 0U;
        double best_distance = metric_distance(*candidate, *seeds.front(), normalization);
        for (std::size_t cluster = 1U; cluster < seeds.size(); ++cluster) {
            const double distance = metric_distance(*candidate, *seeds[cluster], normalization);
            if (distance < best_distance || (distance == best_distance && seeds[cluster]->index < seeds[best_cluster]->index)) {
                best_distance = distance;
                best_cluster = cluster;
            }
        }
        memberships[best_cluster].push_back(candidate);
    }

    std::vector<Cluster> clusters;
    clusters.reserve(memberships.size());
    for (std::size_t cluster_index = 0U; cluster_index < memberships.size(); ++cluster_index) {
        auto& members = memberships[cluster_index];
        std::sort(members.begin(), members.end(), [](const auto* left, const auto* right) { return left->index < right->index; });
        const std::vector<double> centroid = centroid_for(members, normalization);
        const CandidateFeatures* representative = members.front();
        double best_distance = distance_to_centroid(*representative, centroid, normalization);
        for (const CandidateFeatures* candidate : members) {
            const double distance = distance_to_centroid(*candidate, centroid, normalization);
            if (distance < best_distance || (distance == best_distance && candidate->index < representative->index)) {
                representative = candidate;
                best_distance = distance;
            }
        }
        Cluster cluster;
        cluster.cluster_id = cluster_index;
        cluster.representative = representative->index;
        cluster.centroid = centroid;
        for (const CandidateFeatures* member : members) {
            cluster.members.push_back(member->index);
        }
        clusters.push_back(std::move(cluster));
    }
    return core::Result<std::vector<Cluster>, DiversityError>::success(std::move(clusters));
}

core::Result<std::vector<UnusualScore>, DiversityError> rank_unusual(
    const std::vector<CandidateFeatures>& candidates,
    const NormalizationModel& normalization,
    const std::vector<Cluster>& clusters,
    const UnusualBasis basis) {
    if (candidates.empty()) {
        return core::Result<std::vector<UnusualScore>, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings, "unusual ranking requires at least one candidate"));
    }
    const auto all = sorted_candidates(candidates);
    const std::vector<double> population_centroid = centroid_for(all, normalization);
    std::map<core::u64, std::size_t> cluster_for_candidate;
    for (const Cluster& cluster : clusters) {
        for (const core::u64 member : cluster.members) {
            cluster_for_candidate[member] = cluster.cluster_id;
        }
    }

    std::vector<UnusualScore> scores;
    scores.reserve(candidates.size());
    for (const CandidateFeatures* candidate : all) {
        const std::vector<double>* centroid = &population_centroid;
        std::size_t cluster_id = 0U;
        if (basis == UnusualBasis::cluster) {
            const auto assigned = cluster_for_candidate.find(candidate->index);
            if (assigned == cluster_for_candidate.end()) {
                return core::Result<std::vector<UnusualScore>, DiversityError>::failure(error(
                    DiversityErrorCode::candidate_missing,
                    "cluster-based unusual ranking requires every candidate to belong to a cluster"));
            }
            const auto cluster = std::find_if(clusters.begin(), clusters.end(), [assigned](const Cluster& value) {
                return value.cluster_id == assigned->second;
            });
            if (cluster == clusters.end()) {
                return core::Result<std::vector<UnusualScore>, DiversityError>::failure(error(
                    DiversityErrorCode::candidate_missing, "candidate references an absent cluster"));
            }
            centroid = &cluster->centroid;
            cluster_id = cluster->cluster_id;
        }
        UnusualScore score;
        score.index = candidate->index;
        score.cluster_id = cluster_id;
        score.distance = distance_to_centroid(*candidate, *centroid, normalization, &score.contributions);
        scores.push_back(std::move(score));
    }
    std::sort(scores.begin(), scores.end(), [](const auto& left, const auto& right) {
        if (left.distance != right.distance) {
            return left.distance > right.distance;
        }
        return left.index < right.index;
    });
    return core::Result<std::vector<UnusualScore>, DiversityError>::success(std::move(scores));
}

core::Result<std::vector<NeighbourResult>, DiversityError> nearest_neighbours(
    const std::vector<CandidateFeatures>& candidates,
    const NormalizationModel& normalization,
    const core::u64 selected_index,
    const NeighbourSettings& settings) {
    if (!std::isfinite(settings.combined_metric_weight) || settings.combined_metric_weight < 0.0 ||
        settings.combined_metric_weight > 1.0 || !std::isfinite(settings.minimum_metric_diversity) ||
        settings.minimum_metric_diversity < 0.0 || settings.maximum_results == 0U) {
        return core::Result<std::vector<NeighbourResult>, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings,
            "neighbour settings require finite [0,1] combined weight, non-negative diversity floor, and non-zero result limit"));
    }
    const CandidateFeatures* selected = by_index(candidates, selected_index);
    if (selected == nullptr) {
        return core::Result<std::vector<NeighbourResult>, DiversityError>::failure(error(
            DiversityErrorCode::candidate_missing, "selected Quarry candidate is absent"));
    }

    std::vector<NeighbourResult> neighbours;
    for (const CandidateFeatures& candidate : candidates) {
        if (candidate.index == selected_index) {
            continue;
        }
        const double metric = metric_distance(*selected, candidate, normalization);
        if (metric < settings.minimum_metric_diversity) {
            continue;
        }
        const double parameter = parameter_distance(*selected, candidate);
        double distance = metric;
        switch (settings.mode) {
        case NeighbourMode::metric:
            distance = metric;
            break;
        case NeighbourMode::parameter:
            distance = parameter;
            break;
        case NeighbourMode::combined: {
            const double metric_weight = settings.combined_metric_weight;
            distance = std::sqrt(metric_weight * metric * metric + (1.0 - metric_weight) * parameter * parameter);
            break;
        }
        }
        neighbours.push_back(NeighbourResult{candidate.index, distance, metric, parameter});
    }
    std::sort(neighbours.begin(), neighbours.end(), [](const auto& left, const auto& right) {
        if (left.distance != right.distance) {
            return left.distance < right.distance;
        }
        return left.index < right.index;
    });
    if (neighbours.size() > settings.maximum_results) {
        neighbours.resize(settings.maximum_results);
    }
    return core::Result<std::vector<NeighbourResult>, DiversityError>::success(std::move(neighbours));
}

core::Result<std::vector<core::u64>, DiversityError> filter_and_sort_candidates(
    const std::vector<CandidateFeatures>& candidates,
    const std::vector<MetricRangeFilter>& filters,
    const StableSort& sort) {
    for (const MetricRangeFilter& filter : filters) {
        if (filter.name.empty() ||
            (filter.minimum.has_value() && !std::isfinite(*filter.minimum)) ||
            (filter.maximum.has_value() && !std::isfinite(*filter.maximum)) ||
            (filter.minimum.has_value() && filter.maximum.has_value() && *filter.minimum > *filter.maximum)) {
            return core::Result<std::vector<core::u64>, DiversityError>::failure(error(
                DiversityErrorCode::invalid_settings, "metric filters require valid finite ranges"));
        }
    }
    if (sort.kind == StableSortKind::raw_metric && sort.metric.empty()) {
        return core::Result<std::vector<core::u64>, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings, "metric sort requires a metric name"));
    }

    std::vector<const CandidateFeatures*> accepted;
    for (const CandidateFeatures& candidate : candidates) {
        bool include = true;
        for (const MetricRangeFilter& filter : filters) {
            const MetricValue* metric = find_metric(candidate.raw_metrics, filter.name);
            if (metric == nullptr || !std::isfinite(metric->value) ||
                (filter.minimum.has_value() && metric->value < *filter.minimum) ||
                (filter.maximum.has_value() && metric->value > *filter.maximum)) {
                include = false;
                break;
            }
        }
        if (include) {
            accepted.push_back(&candidate);
        }
    }
    std::sort(accepted.begin(), accepted.end(), [&sort](const auto* left, const auto* right) {
        if (sort.kind == StableSortKind::raw_metric) {
            const MetricValue* a = find_metric(left->raw_metrics, sort.metric);
            const MetricValue* b = find_metric(right->raw_metrics, sort.metric);
            if (a != nullptr && b != nullptr && a->value != b->value) {
                return sort.descending ? a->value > b->value : a->value < b->value;
            }
        }
        return left->index < right->index;
    });
    if (sort.kind == StableSortKind::candidate_index && sort.descending) {
        std::reverse(accepted.begin(), accepted.end());
    }
    std::vector<core::u64> output;
    output.reserve(accepted.size());
    for (const CandidateFeatures* candidate : accepted) {
        output.push_back(candidate->index);
    }
    return core::Result<std::vector<core::u64>, DiversityError>::success(std::move(output));
}

core::Result<std::vector<ProjectionPoint>, DiversityError> project_metric_axes(
    const std::vector<CandidateFeatures>& candidates,
    const std::string_view x_metric,
    const std::string_view y_metric) {
    if (x_metric.empty() || y_metric.empty()) {
        return core::Result<std::vector<ProjectionPoint>, DiversityError>::failure(error(
            DiversityErrorCode::invalid_settings, "2D metric projection requires explicit x and y metric axes"));
    }
    std::vector<ProjectionPoint> output;
    output.reserve(candidates.size());
    for (const CandidateFeatures* candidate : sorted_candidates(candidates)) {
        const NormalizedMetric* x = find_normalized(*candidate, x_metric);
        const NormalizedMetric* y = find_normalized(*candidate, y_metric);
        if (x == nullptr || y == nullptr || !x->available || !y->available) {
            continue;
        }
        output.push_back(ProjectionPoint{candidate->index, x->normalized, y->normalized});
    }
    return core::Result<std::vector<ProjectionPoint>, DiversityError>::success(std::move(output));
}

core::Result<core::Recipe, DiversityError> materialize_candidate_recipe(
    const JobManifest& manifest,
    const core::u64 candidate_index) {
    auto recipe = reconstruct_candidate_recipe(manifest, candidate_index);
    if (recipe.is_error()) {
        return core::Result<core::Recipe, DiversityError>::failure(error(
            DiversityErrorCode::quarry_error, recipe.error().message));
    }
    core::Recipe materialized = std::move(recipe).value();
    const std::string fingerprint = core::semantic_fingerprint(materialized);
    const std::string candidate_payload = manifest.identity + "|" + std::to_string(candidate_index) + "|" + fingerprint;
    set_metadata(materialized, "quarry.job_identity", manifest.identity);
    set_metadata(materialized, "quarry.candidate_index", std::to_string(candidate_index));
    set_metadata(materialized, "quarry.candidate_id", core::hex_u64(core::fnv1a64(candidate_payload)));
    set_metadata(materialized, "quarry.source_fingerprint", fingerprint);
    return core::Result<core::Recipe, DiversityError>::success(std::move(materialized));
}

std::vector<core::u8> make_thumbnail_signature(const nodes::Image& image) {
    constexpr core::u32 grid = 8U;
    std::vector<core::u8> signature(grid * grid, 0U);
    if (image.width == 0U || image.height == 0U ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4U) {
        return signature;
    }
    for (core::u32 gy = 0U; gy < grid; ++gy) {
        const core::u32 y0 = static_cast<core::u32>((static_cast<core::u64>(gy) * image.height) / grid);
        core::u32 y1 = static_cast<core::u32>((static_cast<core::u64>(gy + 1U) * image.height) / grid);
        y1 = (std::max)(y1, y0 + 1U);
        y1 = (std::min)(y1, image.height);
        for (core::u32 gx = 0U; gx < grid; ++gx) {
            const core::u32 x0 = static_cast<core::u32>((static_cast<core::u64>(gx) * image.width) / grid);
            core::u32 x1 = static_cast<core::u32>((static_cast<core::u64>(gx + 1U) * image.width) / grid);
            x1 = (std::max)(x1, x0 + 1U);
            x1 = (std::min)(x1, image.width);
            core::u64 sum = 0U;
            core::u64 count = 0U;
            for (core::u32 y = y0; y < y1; ++y) {
                for (core::u32 x = x0; x < x1; ++x) {
                    const std::size_t pixel = (static_cast<std::size_t>(y) * image.width + x) * 4U;
                    const core::u64 luma =
                        (54U * image.rgba[pixel] + 183U * image.rgba[pixel + 1U] + 19U * image.rgba[pixel + 2U] + 128U) >> 8U;
                    sum += (luma * image.rgba[pixel + 3U] + 127U) / 255U;
                    ++count;
                }
            }
            signature[static_cast<std::size_t>(gy) * grid + gx] =
                count == 0U ? 0U : static_cast<core::u8>((sum + count / 2U) / count);
        }
    }
    return signature;
}

double thumbnail_signature_distance(
    const std::vector<core::u8>& left,
    const std::vector<core::u8>& right) noexcept {
    if (left.size() != right.size() || left.empty()) {
        return 1.0;
    }
    core::u64 total = 0U;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        total += left[index] > right[index]
            ? static_cast<core::u64>(left[index] - right[index])
            : static_cast<core::u64>(right[index] - left[index]);
    }
    return static_cast<double>(total) / (255.0 * static_cast<double>(left.size()));
}

}  // namespace artminer::quarry
