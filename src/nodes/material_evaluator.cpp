#include "nodes/material_evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/graph.hpp"
#include "nodes/motion_evaluator.hpp"

namespace artminer::nodes {
namespace {

using core::i64;
using core::u8;
using core::u32;
using core::u64;

struct ScalarField final {
    u32 width{0U};
    u32 height{0U};
    std::vector<double> values;
};

struct HeightField final {
    u32 width{0U};
    u32 height{0U};
    std::vector<double> values;
};

struct MaskField final {
    u32 width{0U};
    u32 height{0U};
    std::vector<u8> values;
};

using Value = std::variant<Image, ScalarField, HeightField, MaskField>;
using EvaluationKey = std::pair<std::string, u64>;

[[nodiscard]] WorkflowError make_error(std::string message) {
    return WorkflowError{std::move(message)};
}

[[nodiscard]] const core::NodeInstance* find_node(
    const core::Recipe& recipe,
    const std::string_view id) noexcept {
    const auto found = std::find_if(
        recipe.nodes.begin(), recipe.nodes.end(),
        [id](const core::NodeInstance& node) { return node.id == id; });
    return found == recipe.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] const core::OutputBinding* find_output(
    const core::Recipe& recipe,
    const std::string_view name) noexcept {
    const auto found = std::find_if(
        recipe.outputs.begin(), recipe.outputs.end(),
        [name](const core::OutputBinding& output) { return output.name == name; });
    return found == recipe.outputs.end() ? nullptr : &*found;
}

[[nodiscard]] const core::ParameterAssignment* find_parameter(
    const core::NodeInstance& node,
    const std::string_view name) noexcept {
    const auto found = std::find_if(
        node.parameters.begin(), node.parameters.end(),
        [name](const core::ParameterAssignment& parameter) { return parameter.name == name; });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] double real_parameter(const core::NodeInstance& node, const std::string_view name) {
    return std::get<double>(find_parameter(node, name)->value);
}

[[nodiscard]] i64 integer_parameter(const core::NodeInstance& node, const std::string_view name) {
    return std::get<i64>(find_parameter(node, name)->value);
}

[[nodiscard]] const std::string& enum_parameter(const core::NodeInstance& node, const std::string_view name) {
    return std::get<std::string>(find_parameter(node, name)->value);
}

[[nodiscard]] double clamp01(const double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] u8 to_unorm8(const double value) noexcept {
    return static_cast<u8>(std::floor(clamp01(value) * 255.0 + 0.5));
}

[[nodiscard]] bool is_workflow_node(const std::string_view type_id) noexcept {
    return type_id.starts_with("core.material.") || type_id.starts_with("core.loop.");
}

[[nodiscard]] double image_channel(
    const Image& image,
    const std::size_t pixel,
    const std::string_view channel) noexcept {
    const std::size_t offset = pixel * 4U;
    const double r = static_cast<double>(image.rgba[offset]) / 255.0;
    const double g = static_cast<double>(image.rgba[offset + 1U]) / 255.0;
    const double b = static_cast<double>(image.rgba[offset + 2U]) / 255.0;
    const double a = static_cast<double>(image.rgba[offset + 3U]) / 255.0;
    if (channel == "r") {
        return r;
    }
    if (channel == "g") {
        return g;
    }
    if (channel == "b") {
        return b;
    }
    if (channel == "a") {
        return a;
    }
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) * a;
}

[[nodiscard]] std::string parameter_text(
    const core::NodeInstance& node,
    const std::string_view name) {
    const auto* parameter = find_parameter(node, name);
    if (parameter == nullptr) {
        return {};
    }
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17);
    if (std::holds_alternative<core::i64>(parameter->value)) {
        stream << std::get<core::i64>(parameter->value);
    } else if (std::holds_alternative<double>(parameter->value)) {
        stream << std::get<double>(parameter->value);
    } else if (std::holds_alternative<bool>(parameter->value)) {
        stream << (std::get<bool>(parameter->value) ? "true" : "false");
    } else {
        stream << std::get<std::string>(parameter->value);
    }
    return stream.str();
}

class WorkflowContext final {
public:
    WorkflowContext(const core::Recipe& recipe, const u64 tick)
        : recipe_(recipe), tick_(tick), width_(recipe.render.width), height_(recipe.render.height) {
        for (const auto& node : recipe.nodes) {
            nodes_.emplace(node.id, &node);
        }
        for (const auto& edge : recipe.edges) {
            incoming_.emplace(std::make_pair(edge.to_node, edge.to_port), &edge);
        }
    }

    [[nodiscard]] core::Result<Value, WorkflowError> evaluate(const std::string& node_id) {
        const EvaluationKey key{node_id, tick_};
        const auto cached = values_.find(key);
        if (cached != values_.end()) {
            return core::Result<Value, WorkflowError>::success(cached->second);
        }
        if (!active_.insert(node_id).second) {
            return core::Result<Value, WorkflowError>::failure(make_error(
                "workflow evaluator encountered an unexpected cycle at node '" + node_id + "'"));
        }

        const auto found = nodes_.find(node_id);
        if (found == nodes_.end()) {
            active_.erase(node_id);
            return core::Result<Value, WorkflowError>::failure(make_error(
                "workflow evaluator could not find node '" + node_id + "'"));
        }
        const core::NodeInstance& node = *found->second;
        core::Result<Value, WorkflowError> result = is_workflow_node(node.type_id)
            ? evaluate_workflow_node(node)
            : evaluate_base_image(node.id, "value");
        active_.erase(node_id);
        if (result.is_error()) {
            return result;
        }
        values_.emplace(key, result.value());
        return result;
    }

    [[nodiscard]] core::Result<Value, WorkflowError> evaluate_port(
        const std::string& node_id,
        const std::string& port) {
        const auto found = nodes_.find(node_id);
        if (found == nodes_.end()) {
            return core::Result<Value, WorkflowError>::failure(make_error(
                "workflow evaluator could not find source node '" + node_id + "'"));
        }
        if (!is_workflow_node(found->second->type_id)) {
            return evaluate_base_image(node_id, port);
        }
        return evaluate(node_id);
    }

private:
    [[nodiscard]] const core::Edge* input_edge(
        const core::NodeInstance& node,
        const std::string_view port) const noexcept {
        const auto found = incoming_.find(std::make_pair(node.id, std::string(port)));
        return found == incoming_.end() ? nullptr : found->second;
    }

    [[nodiscard]] core::Result<Value, WorkflowError> input_value(
        const core::NodeInstance& node,
        const std::string_view port) {
        const core::Edge* edge = input_edge(node, port);
        if (edge == nullptr) {
            return core::Result<Value, WorkflowError>::failure(make_error(
                "workflow node '" + node.id + "' is missing input '" + std::string(port) + "'"));
        }
        return evaluate_port(edge->from_node, edge->from_port);
    }

    [[nodiscard]] core::Result<Value, WorkflowError> evaluate_base_image(
        const std::string& node_id,
        const std::string& port) const {
        core::Recipe isolated = recipe_;
        isolated.outputs.clear();
        isolated.outputs.push_back(core::OutputBinding{"__workflow_source", node_id, port});

        auto animated = render_animation_reference(isolated, tick_, "__workflow_source", nullptr);
        if (animated.is_ok()) {
            return core::Result<Value, WorkflowError>::success(std::move(animated).value());
        }
        auto still = render_reference(isolated, "__workflow_source");
        if (still.is_ok()) {
            return core::Result<Value, WorkflowError>::success(std::move(still).value());
        }
        return core::Result<Value, WorkflowError>::failure(make_error(
            "workflow source '" + node_id + "." + port +
            "' is not an Image output supported by the canonical static or fixed-tick renderer; "
            "height semantics must be introduced by core.material.height_from_image"));
    }

    [[nodiscard]] core::Result<Value, WorkflowError> evaluate_workflow_node(
        const core::NodeInstance& node) {
        if (node.type_id == "core.material.height_from_image") {
            auto source_result = input_value(node, "source");
            if (source_result.is_error()) {
                return source_result;
            }
            if (!std::holds_alternative<Image>(source_result.value())) {
                return core::Result<Value, WorkflowError>::failure(make_error(
                    "height_from_image requires an Image source"));
            }
            const Image& source = std::get<Image>(source_result.value());
            const double minimum = real_parameter(node, "minimum");
            const double maximum = real_parameter(node, "maximum");
            if (maximum <= minimum) {
                return core::Result<Value, WorkflowError>::failure(make_error(
                    "height_from_image maximum must be greater than minimum"));
            }
            const bool invert = enum_parameter(node, "invert") == "yes";
            const std::string& channel = enum_parameter(node, "channel");
            HeightField height{source.width, source.height, {}};
            height.values.resize(static_cast<std::size_t>(source.width) * source.height);
            for (std::size_t pixel = 0U; pixel < height.values.size(); ++pixel) {
                double value = clamp01((image_channel(source, pixel, channel) - minimum) / (maximum - minimum));
                if (invert) {
                    value = 1.0 - value;
                }
                height.values[pixel] = value;
            }
            return core::Result<Value, WorkflowError>::success(std::move(height));
        }

        if (node.type_id == "core.material.height_image") {
            auto height_result = input_value(node, "height");
            if (height_result.is_error()) {
                return height_result;
            }
            if (!std::holds_alternative<HeightField>(height_result.value())) {
                return core::Result<Value, WorkflowError>::failure(make_error(
                    "height_image rejects an untyped ScalarField; connect an explicit core.material.height_from_image conversion"));
            }
            const HeightField& height = std::get<HeightField>(height_result.value());
            Image image{height.width, height.height, {}};
            image.rgba.resize(height.values.size() * 4U);
            for (std::size_t index = 0U; index < height.values.size(); ++index) {
                const u8 value = to_unorm8(height.values[index]);
                image.rgba[index * 4U + 0U] = value;
                image.rgba[index * 4U + 1U] = value;
                image.rgba[index * 4U + 2U] = value;
                image.rgba[index * 4U + 3U] = 255U;
            }
            return core::Result<Value, WorkflowError>::success(std::move(image));
        }

        if (node.type_id == "core.material.normal_from_height") {
            auto height_result = input_value(node, "height");
            if (height_result.is_error()) {
                return height_result;
            }
            if (!std::holds_alternative<HeightField>(height_result.value())) {
                return core::Result<Value, WorkflowError>::failure(make_error(
                    "normal_from_height rejects arbitrary scalar/RGB data; height must come from an explicit core.material.height_from_image node"));
            }
            const HeightField& height = std::get<HeightField>(height_result.value());
            const bool repeat = enum_parameter(node, "edge") == "repeat";
            const bool sobel = enum_parameter(node, "filter") == "sobel";
            const bool left_handed = enum_parameter(node, "handedness") == "left";
            const bool directx = enum_parameter(node, "convention") == "directx";
            const double slope_scale = real_parameter(node, "strength") / real_parameter(node, "texel_scale");

            const auto sample = [&](const int x, const int y) noexcept {
                const int width = static_cast<int>(height.width);
                const int height_extent = static_cast<int>(height.height);
                int sx = x;
                int sy = y;
                if (repeat) {
                    sx = ((sx % width) + width) % width;
                    sy = ((sy % height_extent) + height_extent) % height_extent;
                } else {
                    sx = std::clamp(sx, 0, width - 1);
                    sy = std::clamp(sy, 0, height_extent - 1);
                }
                return height.values[
                    static_cast<std::size_t>(sy) * height.width + static_cast<std::size_t>(sx)];
            };

            Image image{height.width, height.height, {}};
            image.rgba.resize(height.values.size() * 4U);
            for (u32 y = 0U; y < height.height; ++y) {
                for (u32 x = 0U; x < height.width; ++x) {
                    const int ix = static_cast<int>(x);
                    const int iy = static_cast<int>(y);
                    double dx = 0.0;
                    double dy = 0.0;
                    if (sobel) {
                        const double tl = sample(ix - 1, iy - 1);
                        const double t = sample(ix, iy - 1);
                        const double tr = sample(ix + 1, iy - 1);
                        const double l = sample(ix - 1, iy);
                        const double r = sample(ix + 1, iy);
                        const double bl = sample(ix - 1, iy + 1);
                        const double b = sample(ix, iy + 1);
                        const double br = sample(ix + 1, iy + 1);
                        dx = (tr + 2.0 * r + br - tl - 2.0 * l - bl) / 8.0;
                        dy = (bl + 2.0 * b + br - tl - 2.0 * t - tr) / 8.0;
                    } else {
                        dx = (sample(ix + 1, iy) - sample(ix - 1, iy)) * 0.5;
                        dy = (sample(ix, iy + 1) - sample(ix, iy - 1)) * 0.5;
                    }
                    double nx = (left_handed ? dx : -dx) * slope_scale;
                    double ny = -dy * slope_scale;
                    if (directx) {
                        ny = -ny;
                    }
                    double nz = 1.0;
                    const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    nx /= length;
                    ny /= length;
                    nz /= length;
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * height.width + x) * 4U;
                    image.rgba[offset + 0U] = to_unorm8(nx * 0.5 + 0.5);
                    image.rgba[offset + 1U] = to_unorm8(ny * 0.5 + 0.5);
                    image.rgba[offset + 2U] = to_unorm8(nz * 0.5 + 0.5);
                    image.rgba[offset + 3U] = 255U;
                }
            }
            return core::Result<Value, WorkflowError>::success(std::move(image));
        }

        if (node.type_id == "core.material.mask_from_image") {
            auto source_result = input_value(node, "source");
            if (source_result.is_error()) {
                return source_result;
            }
            if (!std::holds_alternative<Image>(source_result.value())) {
                return core::Result<Value, WorkflowError>::failure(make_error(
                    "mask_from_image requires an Image source"));
            }
            const Image& source = std::get<Image>(source_result.value());
            const double threshold = real_parameter(node, "threshold");
            const bool invert = enum_parameter(node, "invert") == "yes";
            const std::string& channel = enum_parameter(node, "channel");
            MaskField mask{source.width, source.height, {}};
            mask.values.resize(static_cast<std::size_t>(source.width) * source.height);
            for (std::size_t pixel = 0U; pixel < mask.values.size(); ++pixel) {
                bool selected = image_channel(source, pixel, channel) >= threshold;
                if (invert) {
                    selected = !selected;
                }
                mask.values[pixel] = selected ? 255U : 0U;
            }
            return core::Result<Value, WorkflowError>::success(std::move(mask));
        }

        if (node.type_id == "core.material.mask_image") {
            auto mask_result = input_value(node, "mask");
            if (mask_result.is_error()) {
                return mask_result;
            }
            if (!std::holds_alternative<MaskField>(mask_result.value())) {
                return core::Result<Value, WorkflowError>::failure(make_error(
                    "mask_image requires a semantic mask source"));
            }
            const MaskField& mask = std::get<MaskField>(mask_result.value());
            Image image{mask.width, mask.height, {}};
            image.rgba.resize(mask.values.size() * 4U);
            for (std::size_t index = 0U; index < mask.values.size(); ++index) {
                image.rgba[index * 4U + 0U] = mask.values[index];
                image.rgba[index * 4U + 1U] = mask.values[index];
                image.rgba[index * 4U + 2U] = mask.values[index];
                image.rgba[index * 4U + 3U] = 255U;
            }
            return core::Result<Value, WorkflowError>::success(std::move(image));
        }

        if (node.type_id == "core.material.pack_masks_rgba") {
            std::array<MaskField, 4U> masks;
            constexpr std::array<std::string_view, 4U> names{"r", "g", "b", "a"};
            for (std::size_t channel = 0U; channel < names.size(); ++channel) {
                auto mask_result = input_value(node, names[channel]);
                if (mask_result.is_error()) {
                    return mask_result;
                }
                if (!std::holds_alternative<MaskField>(mask_result.value())) {
                    return core::Result<Value, WorkflowError>::failure(make_error(
                        "pack_masks_rgba accepts only explicit semantic mask inputs"));
                }
                masks[channel] = std::get<MaskField>(std::move(mask_result).value());
            }
            const u32 width = masks[0].width;
            const u32 height = masks[0].height;
            const std::size_t pixels = static_cast<std::size_t>(width) * height;
            for (const auto& mask : masks) {
                if (mask.width != width || mask.height != height || mask.values.size() != pixels) {
                    return core::Result<Value, WorkflowError>::failure(make_error(
                        "packed mask inputs must have identical dimensions"));
                }
            }
            Image image{width, height, {}};
            image.rgba.resize(pixels * 4U);
            for (std::size_t pixel = 0U; pixel < pixels; ++pixel) {
                for (std::size_t channel = 0U; channel < 4U; ++channel) {
                    image.rgba[pixel * 4U + channel] = masks[channel].values[pixel];
                }
            }
            return core::Result<Value, WorkflowError>::success(std::move(image));
        }

        if (node.type_id == "core.loop.phase") {
            const u64 loop_length = static_cast<u64>(integer_parameter(node, "loop_length"));
            const double phase_offset = real_parameter(node, "phase_offset");
            double phase = static_cast<double>(tick_ % loop_length) / static_cast<double>(loop_length) + phase_offset;
            phase -= std::floor(phase);
            ScalarField field{width_, height_, {}};
            field.values.assign(static_cast<std::size_t>(width_) * height_, phase);
            return core::Result<Value, WorkflowError>::success(std::move(field));
        }

        if (node.type_id == "core.loop.wave_image") {
            auto phase_result = input_value(node, "phase");
            if (phase_result.is_error()) {
                return phase_result;
            }
            if (!std::holds_alternative<ScalarField>(phase_result.value())) {
                return core::Result<Value, WorkflowError>::failure(make_error(
                    "loop wave requires core.loop.phase input"));
            }
            const ScalarField& phase_field = std::get<ScalarField>(phase_result.value());
            const double phase = phase_field.values.empty() ? 0.0 : phase_field.values.front();
            const bool axis_x = enum_parameter(node, "axis") == "x";
            const i64 spatial_cycles = integer_parameter(node, "spatial_cycles");
            const i64 phase_cycles = integer_parameter(node, "phase_cycles");
            const double contrast = real_parameter(node, "contrast");
            Image image{width_, height_, {}};
            image.rgba.resize(static_cast<std::size_t>(width_) * height_ * 4U);
            for (u32 y = 0U; y < height_; ++y) {
                const double v = height_ <= 1U ? 0.0 :
                    static_cast<double>(y) / static_cast<double>(height_ - 1U);
                for (u32 x = 0U; x < width_; ++x) {
                    const double u = width_ <= 1U ? 0.0 :
                        static_cast<double>(x) / static_cast<double>(width_ - 1U);
                    const double coordinate = axis_x ? u : v;
                    const double angle = 2.0 * std::numbers::pi_v<double> *
                        (coordinate * static_cast<double>(spatial_cycles) +
                         phase * static_cast<double>(phase_cycles));
                    const double centered = 0.5 + 0.5 * std::sin(angle);
                    const double value = clamp01((centered - 0.5) * contrast + 0.5);
                    const u8 byte = to_unorm8(value);
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * width_ + x) * 4U;
                    image.rgba[offset + 0U] = byte;
                    image.rgba[offset + 1U] = byte;
                    image.rgba[offset + 2U] = byte;
                    image.rgba[offset + 3U] = 255U;
                }
            }
            return core::Result<Value, WorkflowError>::success(std::move(image));
        }

        return core::Result<Value, WorkflowError>::failure(make_error(
            "unsupported AM-013 workflow node type '" + node.type_id + "'"));
    }

    const core::Recipe& recipe_;
    u64 tick_{0U};
    u32 width_{0U};
    u32 height_{0U};
    std::map<std::string, const core::NodeInstance*, std::less<>> nodes_;
    std::map<std::pair<std::string, std::string>, const core::Edge*> incoming_;
    std::map<EvaluationKey, Value> values_;
    std::set<std::string, std::less<>> active_;
};

[[nodiscard]] core::Result<Image, WorkflowError> render_workflow_impl(
    const core::Recipe& recipe,
    const u64 tick,
    const std::string_view output_name,
    const bool allow_ordinary_animation) {
    const auto errors = core::validate_recipe(recipe);
    if (!errors.empty()) {
        return core::Result<Image, WorkflowError>::failure(make_error(
            "workflow render requires a valid recipe: " + errors.front().message));
    }
    const core::OutputBinding* output = find_output(recipe, output_name);
    if (output == nullptr) {
        return core::Result<Image, WorkflowError>::failure(make_error(
            "recipe does not define output '" + std::string(output_name) + "'"));
    }
    const core::NodeInstance* root = find_node(recipe, output->node_id);
    if (root == nullptr) {
        return core::Result<Image, WorkflowError>::failure(make_error(
            "workflow output references a missing root node"));
    }

    if (!is_workflow_node(root->type_id)) {
        if (allow_ordinary_animation) {
            auto animated = render_animation_reference(recipe, tick, output_name, nullptr);
            if (animated.is_ok()) {
                return core::Result<Image, WorkflowError>::success(std::move(animated).value());
            }
        }
        auto still = render_reference(recipe, output_name);
        if (still.is_ok()) {
            return core::Result<Image, WorkflowError>::success(std::move(still).value());
        }
        return core::Result<Image, WorkflowError>::failure(make_error(
            "ordinary output could not be rendered by the canonical evaluator"));
    }

    WorkflowContext context(recipe, tick);
    auto value = context.evaluate_port(output->node_id, output->port);
    if (value.is_error()) {
        return core::Result<Image, WorkflowError>::failure(value.error());
    }
    if (!std::holds_alternative<Image>(value.value())) {
        return core::Result<Image, WorkflowError>::failure(make_error(
            "workflow output '" + std::string(output_name) + "' must resolve to an Image"));
    }
    return core::Result<Image, WorkflowError>::success(std::get<Image>(std::move(value).value()));
}

[[nodiscard]] double image_distance(const Image& a, const Image& b) noexcept {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size() || a.rgba.empty()) {
        return 1.0;
    }
    double difference = 0.0;
    for (std::size_t index = 0U; index < a.rgba.size(); ++index) {
        difference += std::abs(static_cast<int>(a.rgba[index]) - static_cast<int>(b.rgba[index]));
    }
    return difference / (255.0 * static_cast<double>(a.rgba.size()));
}

[[nodiscard]] double transition_distance(
    const Image& a0,
    const Image& a1,
    const Image& b0,
    const Image& b1) noexcept {
    if (a0.width != a1.width || a0.width != b0.width || a0.width != b1.width ||
        a0.height != a1.height || a0.height != b0.height || a0.height != b1.height ||
        a0.rgba.size() != a1.rgba.size() || a0.rgba.size() != b0.rgba.size() ||
        a0.rgba.size() != b1.rgba.size() || a0.rgba.empty()) {
        return 1.0;
    }
    double difference = 0.0;
    for (std::size_t index = 0U; index < a0.rgba.size(); ++index) {
        const int first = static_cast<int>(a1.rgba[index]) - static_cast<int>(a0.rgba[index]);
        const int boundary = static_cast<int>(b1.rgba[index]) - static_cast<int>(b0.rgba[index]);
        difference += std::abs(first - boundary);
    }
    return difference / (510.0 * static_cast<double>(a0.rgba.size()));
}

[[nodiscard]] std::set<std::string, std::less<>> reachable_nodes(
    const core::Recipe& recipe,
    const core::OutputBinding& output) {
    std::set<std::string, std::less<>> reachable;
    std::vector<std::string> pending{output.node_id};
    while (!pending.empty()) {
        std::string node = std::move(pending.back());
        pending.pop_back();
        if (!reachable.insert(node).second) {
            continue;
        }
        for (const auto& edge : recipe.edges) {
            if (edge.to_node == node) {
                pending.push_back(edge.from_node);
            }
        }
    }
    return reachable;
}

[[nodiscard]] std::string edge_source_text(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const std::string_view port) {
    const auto found = std::find_if(
        recipe.edges.begin(), recipe.edges.end(),
        [&](const core::Edge& edge) { return edge.to_node == node.id && edge.to_port == port; });
    if (found == recipe.edges.end()) {
        return "<missing>";
    }
    return found->from_node + "." + found->from_port;
}

}  // namespace

core::Result<Image, WorkflowError> render_workflow_reference(
    const core::Recipe& recipe,
    const std::string_view output_name) {
    return render_workflow_impl(recipe, 0U, output_name, false);
}

core::Result<Image, WorkflowError> render_workflow_tick_reference(
    const core::Recipe& recipe,
    const u64 tick,
    const std::string_view output_name) {
    if (tick > kMaximumAnimationTick) {
        return core::Result<Image, WorkflowError>::failure(make_error(
            "workflow tick exceeds the canonical animation limit"));
    }
    return render_workflow_impl(recipe, tick, output_name, true);
}

core::Result<LoopValidation, WorkflowError> validate_workflow_loop(
    const core::Recipe& recipe,
    const u64 loop_length,
    const std::string_view output_name,
    const double tolerance) {
    if (loop_length < 2U || loop_length >= kMaximumAnimationTick) {
        return core::Result<LoopValidation, WorkflowError>::failure(make_error(
            "loop length must be in [2, 16383] so the endpoint and following transition can both be checked"));
    }
    if (!std::isfinite(tolerance) || tolerance < 0.0 || tolerance > 1.0) {
        return core::Result<LoopValidation, WorkflowError>::failure(make_error(
            "loop continuity tolerance must be finite and within [0, 1]"));
    }
    const core::OutputBinding* output = find_output(recipe, output_name);
    if (output == nullptr) {
        return core::Result<LoopValidation, WorkflowError>::failure(make_error(
            "recipe does not define loop output '" + std::string(output_name) + "'"));
    }

    LoopValidation validation;
    validation.loop_length = loop_length;
    validation.tolerance = tolerance;
    bool unsupported_state = false;
    bool incompatible_phase = false;
    const auto reachable = reachable_nodes(recipe, *output);
    for (const std::string& id : reachable) {
        const core::NodeInstance* node = find_node(recipe, id);
        if (node == nullptr) {
            continue;
        }
        const core::NodeMetadata* metadata = core::builtin_node_registry().find(node->type_id);
        if (metadata == nullptr || metadata->state_class == core::NodeStateClass::stateless) {
            continue;
        }
        validation.contains_stateful_nodes = true;
        if (node->type_id == "core.loop.phase") {
            const u64 native_period = static_cast<u64>(integer_parameter(*node, "loop_length"));
            if (native_period == 0U || loop_length % native_period != 0U) {
                incompatible_phase = true;
            }
        } else {
            unsupported_state = true;
        }
    }

    auto start = render_workflow_tick_reference(recipe, 0U, output_name);
    auto endpoint = render_workflow_tick_reference(recipe, loop_length, output_name);
    auto first = render_workflow_tick_reference(recipe, 1U, output_name);
    auto after_endpoint = render_workflow_tick_reference(recipe, loop_length + 1U, output_name);
    if (start.is_error()) {
        return core::Result<LoopValidation, WorkflowError>::failure(start.error());
    }
    if (endpoint.is_error()) {
        return core::Result<LoopValidation, WorkflowError>::failure(endpoint.error());
    }
    if (first.is_error()) {
        return core::Result<LoopValidation, WorkflowError>::failure(first.error());
    }
    if (after_endpoint.is_error()) {
        return core::Result<LoopValidation, WorkflowError>::failure(after_endpoint.error());
    }
    validation.endpoint_error = image_distance(start.value(), endpoint.value());
    validation.transition_error = transition_distance(
        start.value(), first.value(), endpoint.value(), after_endpoint.value());

    if (unsupported_state) {
        validation.reason =
            "image endpoints were measured, but reachable accumulated/state-boundary nodes do not have an AM-013 state-closure proof; sequence repetition is not labelled a perfect loop";
    } else if (incompatible_phase) {
        validation.reason =
            "requested loop length is not an integer multiple of every reachable core.loop.phase native period";
    } else if (validation.endpoint_error > tolerance) {
        validation.reason = "endpoint image continuity exceeds the configured tolerance";
    } else if (validation.transition_error > tolerance) {
        validation.reason = "boundary transition continuity exceeds the configured tolerance";
    } else {
        validation.validated = true;
        validation.reason = "endpoint, transition, and reachable-state proof checks passed";
    }
    return core::Result<LoopValidation, WorkflowError>::success(std::move(validation));
}

core::Result<MaterialOutputDescription, WorkflowError> describe_workflow_output(
    const core::Recipe& recipe,
    const std::string_view output_name) {
    const core::OutputBinding* output = find_output(recipe, output_name);
    if (output == nullptr) {
        return core::Result<MaterialOutputDescription, WorkflowError>::failure(make_error(
            "recipe does not define output '" + std::string(output_name) + "'"));
    }
    const core::NodeInstance* root = find_node(recipe, output->node_id);
    if (root == nullptr) {
        return core::Result<MaterialOutputDescription, WorkflowError>::failure(make_error(
            "workflow output root is missing"));
    }

    MaterialOutputDescription description;
    if (root->type_id == "core.material.height_image") {
        description.semantic = "height";
        description.mappings.push_back({"height", edge_source_text(recipe, *root, "height")});
    } else if (root->type_id == "core.material.normal_from_height") {
        description.semantic = "normal";
        description.mappings.push_back({"height", edge_source_text(recipe, *root, "height")});
        for (const std::string_view parameter : {"filter", "edge", "strength", "texel_scale", "handedness", "convention"}) {
            description.settings.push_back({std::string(parameter), parameter_text(*root, parameter)});
        }
    } else if (root->type_id == "core.material.mask_image") {
        description.semantic = "mask";
        description.mappings.push_back({"mask", edge_source_text(recipe, *root, "mask")});
    } else if (root->type_id == "core.material.pack_masks_rgba") {
        description.semantic = "packed-mask";
        for (const std::string_view channel : {"r", "g", "b", "a"}) {
            description.mappings.push_back({std::string(channel), edge_source_text(recipe, *root, channel)});
        }
    } else if (root->type_id == "core.loop.wave_image") {
        description.semantic = "loop-image";
        description.mappings.push_back({"phase", edge_source_text(recipe, *root, "phase")});
        for (const std::string_view parameter : {"axis", "spatial_cycles", "phase_cycles", "contrast"}) {
            description.settings.push_back({std::string(parameter), parameter_text(*root, parameter)});
        }
    }

    // Record the explicit image-to-height/mask conversion details when they are
    // one edge upstream so exported material provenance includes the semantic
    // source operation rather than merely the final carrier port.
    for (const auto& edge : recipe.edges) {
        if (edge.to_node != root->id) {
            continue;
        }
        const core::NodeInstance* source = find_node(recipe, edge.from_node);
        if (source == nullptr) {
            continue;
        }
        if (source->type_id == "core.material.height_from_image") {
            description.settings.push_back({"height.channel", parameter_text(*source, "channel")});
            description.settings.push_back({"height.minimum", parameter_text(*source, "minimum")});
            description.settings.push_back({"height.maximum", parameter_text(*source, "maximum")});
            description.settings.push_back({"height.invert", parameter_text(*source, "invert")});
        }
    }
    return core::Result<MaterialOutputDescription, WorkflowError>::success(std::move(description));
}

}  // namespace artminer::nodes
