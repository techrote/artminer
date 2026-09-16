#include "gpu/d3d11_preview.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include "core/graph.hpp"

namespace artminer::gpu {
namespace {

using Microsoft::WRL::ComPtr;
using core::i64;
using core::u8;
using core::u32;

constexpr std::string_view kFullscreenShader = R"HLSL(
struct VsOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOutput VSMain(uint vertex_id : SV_VertexID) {
    VsOutput output;
    float2 uv = float2((vertex_id << 1U) & 2U, vertex_id & 2U);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

Texture2D preview_texture : register(t0);
SamplerState preview_sampler : register(s0);

float4 PSCopy(VsOutput input) : SV_Target {
    return preview_texture.Sample(preview_sampler, input.uv);
}
)HLSL";

[[nodiscard]] PreviewError make_error(std::string message) {
    return PreviewError{std::move(message)};
}

[[nodiscard]] std::string hresult_text(const char* operation, const HRESULT hr) {
    std::ostringstream stream;
    stream << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(hr) << ')';
    return stream.str();
}

[[nodiscard]] core::Result<ComPtr<ID3DBlob>, PreviewError> compile_shader(
    const std::string_view source,
    const char* entry,
    const char* target) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> diagnostics;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT hr = D3DCompile(
        source.data(),
        source.size(),
        "ArtMinerGeneratedPreview",
        nullptr,
        nullptr,
        entry,
        target,
        flags,
        0U,
        bytecode.GetAddressOf(),
        diagnostics.GetAddressOf());
    if (FAILED(hr)) {
        std::string message = hresult_text("D3DCompile", hr);
        if (diagnostics != nullptr && diagnostics->GetBufferPointer() != nullptr) {
            message += ": ";
            message.append(
                static_cast<const char*>(diagnostics->GetBufferPointer()),
                diagnostics->GetBufferSize());
        }
        return core::Result<ComPtr<ID3DBlob>, PreviewError>::failure(make_error(std::move(message)));
    }
    return core::Result<ComPtr<ID3DBlob>, PreviewError>::success(std::move(bytecode));
}

[[nodiscard]] const core::ParameterAssignment* find_parameter(
    const core::NodeInstance& node,
    const std::string_view name) noexcept {
    const auto found = std::find_if(
        node.parameters.begin(),
        node.parameters.end(),
        [&](const core::ParameterAssignment& assignment) { return assignment.name == name; });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] double real_parameter(const core::NodeInstance& node, const std::string_view name) {
    return std::get<double>(find_parameter(node, name)->value);
}

[[nodiscard]] i64 integer_parameter(const core::NodeInstance& node, const std::string_view name) {
    return std::get<i64>(find_parameter(node, name)->value);
}

[[nodiscard]] const std::string& enum_parameter(
    const core::NodeInstance& node,
    const std::string_view name) {
    return std::get<std::string>(find_parameter(node, name)->value);
}

[[nodiscard]] std::string float_literal(const double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << value;
    std::string text = stream.str();
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    return text;
}

struct ColourLiteral final {
    double r{0.0};
    double g{0.0};
    double b{0.0};
    double a{1.0};
};

[[nodiscard]] std::string colour_literal(const ColourLiteral& colour) {
    return "float4(" + float_literal(colour.r) + "," + float_literal(colour.g) + "," +
        float_literal(colour.b) + "," + float_literal(colour.a) + ")";
}

class ShaderPlanBuilder final {
public:
    explicit ShaderPlanBuilder(const core::Recipe& recipe) : recipe_(recipe) {
        for (const auto& node : recipe.nodes) {
            nodes_.emplace(node.id, &node);
        }
        for (const auto& edge : recipe.edges) {
            incoming_.emplace(std::make_pair(edge.to_node, edge.to_port), &edge);
        }
    }

    [[nodiscard]] PreviewPlan build() {
        const auto output = std::find_if(
            recipe_.outputs.begin(),
            recipe_.outputs.end(),
            [](const core::OutputBinding& binding) { return binding.name == "main"; });
        if (output == recipe_.outputs.end()) {
            add_unsupported("<missing output main>");
            return finish({});
        }
        const auto node = nodes_.find(output->node_id);
        if (node == nodes_.end()) {
            add_unsupported("<missing output node>");
            return finish({});
        }

        std::string colour = emit_image(*node->second, "uv", "pixel");
        if (!unsupported_.empty() || colour.empty()) {
            return finish({});
        }

        std::ostringstream shader;
        shader.imbue(std::locale::classic());
        shader << R"HLSL(
float artminer_box_sdf(float2 p, float2 center, float2 half_size) {
    float2 q = abs(p - center) - half_size;
    float2 outside_part = max(q, float2(0.0, 0.0));
    return length(outside_part) + min(max(q.x, q.y), 0.0);
}

float4 artminer_heat(float value) {
    float t = saturate(value);
    return float4(
        saturate(1.5 - abs(4.0 * t - 3.0)),
        saturate(1.5 - abs(4.0 * t - 2.0)),
        saturate(1.5 - abs(4.0 * t - 1.0)),
        1.0);
}

float4 artminer_palette2_linear(float value, float4 c0, float4 c1) {
    return lerp(c0, c1, saturate(value));
}

float4 artminer_palette2_nearest(float value, float4 c0, float4 c1) {
    return saturate(value) < 0.5 ? c0 : c1;
}

float4 artminer_palette4_linear(float value, float4 c0, float4 c1, float4 c2, float4 c3) {
    float scaled = saturate(value) * 3.0;
    if (scaled < 1.0) {
        return lerp(c0, c1, scaled);
    }
    if (scaled < 2.0) {
        return lerp(c1, c2, scaled - 1.0);
    }
    return lerp(c2, c3, scaled - 2.0);
}

float4 artminer_palette4_nearest(float value, float4 c0, float4 c1, float4 c2, float4 c3) {
    uint index = (uint)floor(saturate(value) * 3.0 + 0.5);
    if (index == 0U) return c0;
    if (index == 1U) return c1;
    if (index == 2U) return c2;
    return c3;
}

uint artminer_bayer4(uint2 pixel) {
    static const uint bayer_values[16] = {
        0U, 8U, 2U, 10U,
        12U, 4U, 14U, 6U,
        3U, 11U, 1U, 9U,
        15U, 7U, 13U, 5U
    };
    return bayer_values[(pixel.y & 3U) * 4U + (pixel.x & 3U)];
}

uint artminer_bayer8(uint2 pixel) {
    uint bx = pixel.x & 7U;
    uint by = pixel.y & 7U;
    uint x0 = bx & 1U;
    uint x1 = (bx >> 1U) & 1U;
    uint x2 = (bx >> 2U) & 1U;
    uint y0 = by & 1U;
    uint y1 = (by >> 1U) & 1U;
    uint y2 = (by >> 2U) & 1U;
    return ((x0 ^ y0) << 5U) | (y0 << 4U) | ((x1 ^ y1) << 3U) |
        (y1 << 2U) | ((x2 ^ y2) << 1U) | y2;
}

float artminer_dither_channel(float value, float levels, float threshold) {
    float scaled = saturate(value) * (levels - 1.0);
    float base = floor(scaled);
    float fraction = scaled - base;
    float quantized = base + (fraction > threshold ? 1.0 : 0.0);
    return saturate(quantized / (levels - 1.0));
}

float4 PSMain(float4 position : SV_Position) : SV_Target {
)HLSL";
        shader << "    float2 uv = position.xy / float2(" << recipe_.render.width << ".0,"
               << recipe_.render.height << ".0);\n";
        shader << "    uint2 pixel = uint2(position.xy);\n";
        shader << "    return saturate(" << colour << ");\n";
        shader << "}\n";
        return finish(shader.str());
    }

private:
    [[nodiscard]] PreviewPlan finish(std::string source) {
        std::sort(unsupported_.begin(), unsupported_.end());
        unsupported_.erase(std::unique(unsupported_.begin(), unsupported_.end()), unsupported_.end());
        return PreviewPlan{unsupported_.empty() && !source.empty(), std::move(source), unsupported_};
    }

    void add_unsupported(std::string value) {
        unsupported_.push_back(std::move(value));
    }

    [[nodiscard]] const core::NodeInstance* source_node(
        const core::NodeInstance& node,
        const std::string_view port) {
        const auto edge = incoming_.find(std::make_pair(node.id, std::string(port)));
        if (edge == incoming_.end()) {
            add_unsupported(node.type_id + ":missing-input:" + std::string(port));
            return nullptr;
        }
        const auto source = nodes_.find(edge->second->from_node);
        if (source == nodes_.end()) {
            add_unsupported(node.type_id + ":missing-source");
            return nullptr;
        }
        return source->second;
    }

    [[nodiscard]] std::string emit_scalar(const core::NodeInstance& node, const std::string& uv) {
        if (active_.contains(node.id)) {
            add_unsupported(node.type_id + ":cycle");
            return {};
        }
        active_.insert(node.id);
        const auto leave = [this, &node]() { active_.erase(node.id); };

        std::string result;
        if (node.type_id == "core.scalar.constant") {
            result = float_literal(real_parameter(node, "value"));
        } else if (node.type_id == "core.scalar.pass") {
            if (const auto* source = source_node(node, "source")) {
                result = emit_scalar(*source, uv);
            }
        } else if (node.type_id == "core.scalar.coord_x") {
            result = "(" + uv + ").x";
        } else if (node.type_id == "core.scalar.coord_y") {
            result = "(" + uv + ").y";
        } else if (node.type_id == "core.scalar.radial") {
            const std::string center = "float2(" + float_literal(real_parameter(node, "center_x")) + "," +
                float_literal(real_parameter(node, "center_y")) + ")";
            result = "(length((" + uv + "-" + center + ")*2.0)*" +
                float_literal(real_parameter(node, "scale")) + ")";
        } else if (node.type_id == "core.scalar.angular") {
            const std::string center = "float2(" + float_literal(real_parameter(node, "center_x")) + "," +
                float_literal(real_parameter(node, "center_y")) + ")";
            result = "frac((atan2((" + uv + ").y-" + center + ".y,(" + uv + ").x-" + center +
                ".x)/(2.0*3.14159265358979323846)+0.5)*" +
                float_literal(real_parameter(node, "turns")) + "+" +
                float_literal(real_parameter(node, "phase")) + ")";
        } else if (node.type_id == "core.sdf.circle") {
            const std::string center = "float2(" + float_literal(real_parameter(node, "center_x")) + "," +
                float_literal(real_parameter(node, "center_y")) + ")";
            result = "(length(" + uv + "-" + center + ")-" + float_literal(real_parameter(node, "radius")) + ")";
        } else if (node.type_id == "core.sdf.box") {
            const std::string center = "float2(" + float_literal(real_parameter(node, "center_x")) + "," +
                float_literal(real_parameter(node, "center_y")) + ")";
            const std::string half_size = "float2(" + float_literal(real_parameter(node, "half_width")) + "," +
                float_literal(real_parameter(node, "half_height")) + ")";
            result = "artminer_box_sdf(" + uv + "," + center + "," + half_size + ")";
        } else if (node.type_id == "core.scalar.minimum" || node.type_id == "core.scalar.maximum") {
            const auto* a = source_node(node, "a");
            const auto* b = source_node(node, "b");
            if (a != nullptr && b != nullptr) {
                const std::string ae = emit_scalar(*a, uv);
                const std::string be = emit_scalar(*b, uv);
                if (!ae.empty() && !be.empty()) {
                    result = std::string(node.type_id == "core.scalar.minimum" ? "min(" : "max(") + ae + "," + be + ")";
                }
            }
        } else if (node.type_id == "core.scalar.threshold") {
            if (const auto* source = source_node(node, "source")) {
                const std::string input = emit_scalar(*source, uv);
                if (!input.empty()) {
                    result = "((" + input + ")>=" + float_literal(real_parameter(node, "threshold")) + "?" +
                        float_literal(real_parameter(node, "high")) + ":" +
                        float_literal(real_parameter(node, "low")) + ")";
                }
            }
        } else if (node.type_id == "core.scalar.quantize") {
            if (const auto* source = source_node(node, "source")) {
                const std::string input = emit_scalar(*source, uv);
                const double minimum = real_parameter(node, "minimum");
                const double maximum = real_parameter(node, "maximum");
                const i64 levels = integer_parameter(node, "levels");
                if (maximum > minimum && !input.empty()) {
                    const std::string normalized = "saturate((" + input + "-" + float_literal(minimum) + ")/(" +
                        float_literal(maximum) + "-" + float_literal(minimum) + "))";
                    result = "(" + float_literal(minimum) + "+(floor((" + normalized + ")*" +
                        float_literal(static_cast<double>(levels - 1)) + "+0.5)/" +
                        float_literal(static_cast<double>(levels - 1)) + ")*(" + float_literal(maximum) + "-" +
                        float_literal(minimum) + "))";
                }
            }
        } else if (node.type_id == "core.scalar.from_colour") {
            if (const auto* source = source_node(node, "source")) {
                const std::string colour = emit_colour(*source, uv);
                if (!colour.empty()) {
                    const std::string& channel = enum_parameter(node, "channel");
                    if (channel == "r" || channel == "g" || channel == "b" || channel == "a") {
                        result = "(" + colour + ")." + channel;
                    } else {
                        result = "dot((" + colour + ").rgb,float3(0.2126,0.7152,0.0722))";
                    }
                }
            }
        } else {
            // Sampling transforms and seeded noise remain CPU-only in AM-004.
            // Their canonical evaluator materializes intermediate rasters, so a
            // direct analytic shader would not be an honest equivalence claim.
            add_unsupported(node.type_id);
        }

        leave();
        return result;
    }

    [[nodiscard]] std::vector<ColourLiteral> palette_colours(const core::NodeInstance& node) {
        if (node.type_id == "core.palette.gradient2") {
            return {
                {real_parameter(node, "r0"), real_parameter(node, "g0"), real_parameter(node, "b0"), real_parameter(node, "a0")},
                {real_parameter(node, "r1"), real_parameter(node, "g1"), real_parameter(node, "b1"), real_parameter(node, "a1")},
            };
        }
        if (node.type_id == "core.palette.default") {
            const std::string& preset = enum_parameter(node, "preset");
            if (preset == "mono") {
                return {{0.0, 0.0, 0.0, 1.0}, {1.0, 1.0, 1.0, 1.0}};
            }
            if (preset == "warm") {
                return {
                    {0.055, 0.016, 0.10, 1.0},
                    {0.45, 0.06, 0.08, 1.0},
                    {0.90, 0.32, 0.08, 1.0},
                    {1.0, 0.82, 0.28, 1.0},
                };
            }
            return {
                {0.01, 0.04, 0.12, 1.0},
                {0.02, 0.22, 0.42, 1.0},
                {0.08, 0.62, 0.72, 1.0},
                {0.72, 0.95, 0.98, 1.0},
            };
        }
        add_unsupported(node.type_id);
        return {};
    }

    [[nodiscard]] std::string emit_palette_sample(
        const core::NodeInstance& palette,
        const std::string& scalar,
        const bool nearest) {
        const auto colours = palette_colours(palette);
        if (colours.size() == 2U) {
            return std::string(nearest ? "artminer_palette2_nearest(" : "artminer_palette2_linear(") + scalar + "," +
                colour_literal(colours[0]) + "," + colour_literal(colours[1]) + ")";
        }
        if (colours.size() == 4U) {
            return std::string(nearest ? "artminer_palette4_nearest(" : "artminer_palette4_linear(") + scalar + "," +
                colour_literal(colours[0]) + "," + colour_literal(colours[1]) + "," +
                colour_literal(colours[2]) + "," + colour_literal(colours[3]) + ")";
        }
        return {};
    }

    [[nodiscard]] std::string emit_colour(const core::NodeInstance& node, const std::string& uv) {
        if (node.type_id == "core.colour.compose") {
            const auto* r = source_node(node, "r");
            const auto* g = source_node(node, "g");
            const auto* b = source_node(node, "b");
            const auto* a = source_node(node, "a");
            if (r == nullptr || g == nullptr || b == nullptr || a == nullptr) {
                return {};
            }
            const std::string re = emit_scalar(*r, uv);
            const std::string ge = emit_scalar(*g, uv);
            const std::string be = emit_scalar(*b, uv);
            const std::string ae = emit_scalar(*a, uv);
            if (re.empty() || ge.empty() || be.empty() || ae.empty()) {
                return {};
            }
            return "float4(" + re + "," + ge + "," + be + "," + ae + ")";
        }
        if (node.type_id == "core.colour.from_palette") {
            const auto* source = source_node(node, "source");
            const auto* palette = source_node(node, "palette");
            if (source == nullptr || palette == nullptr) {
                return {};
            }
            const std::string scalar = emit_scalar(*source, uv);
            if (scalar.empty()) {
                return {};
            }
            return emit_palette_sample(*palette, scalar, enum_parameter(node, "mode") == "nearest");
        }
        add_unsupported(node.type_id);
        return {};
    }

    [[nodiscard]] std::string emit_image(
        const core::NodeInstance& node,
        const std::string& uv,
        const std::string& pixel) {
        if (node.type_id == "core.image.from_colour") {
            if (const auto* source = source_node(node, "source")) {
                return emit_colour(*source, uv);
            }
            return {};
        }
        if (node.type_id == "core.image.from_scalar") {
            if (const auto* source = source_node(node, "source")) {
                const std::string scalar = emit_scalar(*source, uv);
                if (scalar.empty()) {
                    return {};
                }
                if (enum_parameter(node, "palette") == "heat") {
                    return "artminer_heat(" + scalar + ")";
                }
                return "float4(saturate(" + scalar + ").xxx,1.0)";
            }
            return {};
        }
        if (node.type_id == "core.image.ordered_dither") {
            if (const auto* source = source_node(node, "source")) {
                const std::string colour = emit_colour(*source, uv);
                if (colour.empty()) {
                    return {};
                }
                const i64 levels = integer_parameter(node, "levels");
                const bool bayer8 = enum_parameter(node, "matrix") == "bayer8";
                const std::string threshold = "((float)" + std::string(bayer8 ? "artminer_bayer8(" : "artminer_bayer4(") +
                    pixel + ")+0.5)/" + std::string(bayer8 ? "64.0" : "16.0") + ")";
                const std::string level_text = float_literal(static_cast<double>(levels));
                return "float4(artminer_dither_channel((" + colour + ").r," + level_text + "," + threshold + ")," +
                    "artminer_dither_channel((" + colour + ").g," + level_text + "," + threshold + ")," +
                    "artminer_dither_channel((" + colour + ").b," + level_text + "," + threshold + ")," +
                    "saturate((" + colour + ").a))";
            }
            return {};
        }
        add_unsupported(node.type_id);
        return {};
    }

    const core::Recipe& recipe_;
    std::map<std::string, const core::NodeInstance*, std::less<>> nodes_;
    std::map<std::pair<std::string, std::string>, const core::Edge*> incoming_;
    std::set<std::string, std::less<>> active_;
    std::vector<std::string> unsupported_;
};

struct DeviceBundle final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    bool warp{false};
};

[[nodiscard]] core::Result<DeviceBundle, PreviewError> create_offscreen_device(const bool force_warp) {
    DeviceBundle bundle;
    D3D_FEATURE_LEVEL feature_level{};
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    const D3D_DRIVER_TYPE driver = force_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        driver,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        1U,
        D3D11_SDK_VERSION,
        bundle.device.GetAddressOf(),
        &feature_level,
        bundle.context.GetAddressOf());
    if (FAILED(hr) && !force_warp) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            1U,
            D3D11_SDK_VERSION,
            bundle.device.ReleaseAndGetAddressOf(),
            &feature_level,
            bundle.context.ReleaseAndGetAddressOf());
        bundle.warp = SUCCEEDED(hr);
    } else {
        bundle.warp = force_warp;
    }
    if (FAILED(hr)) {
        return core::Result<DeviceBundle, PreviewError>::failure(make_error(hresult_text("D3D11CreateDevice", hr)));
    }
    return core::Result<DeviceBundle, PreviewError>::success(std::move(bundle));
}

[[nodiscard]] core::Result<ComPtr<ID3D11VertexShader>, PreviewError> create_fullscreen_vertex_shader(
    ID3D11Device* device) {
    auto compiled = compile_shader(kFullscreenShader, "VSMain", "vs_5_0");
    if (compiled.is_error()) {
        return core::Result<ComPtr<ID3D11VertexShader>, PreviewError>::failure(compiled.error());
    }
    ComPtr<ID3D11VertexShader> shader;
    const HRESULT hr = device->CreateVertexShader(
        compiled.value()->GetBufferPointer(),
        compiled.value()->GetBufferSize(),
        nullptr,
        shader.GetAddressOf());
    if (FAILED(hr)) {
        return core::Result<ComPtr<ID3D11VertexShader>, PreviewError>::failure(make_error(hresult_text("CreateVertexShader", hr)));
    }
    return core::Result<ComPtr<ID3D11VertexShader>, PreviewError>::success(std::move(shader));
}

struct RenderedTexture final {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
};

[[nodiscard]] core::Result<RenderedTexture, PreviewError> render_plan_to_texture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const core::Recipe& recipe,
    const PreviewPlan& plan) {
    auto vertex = create_fullscreen_vertex_shader(device);
    if (vertex.is_error()) {
        return core::Result<RenderedTexture, PreviewError>::failure(vertex.error());
    }
    auto pixel_bytecode = compile_shader(plan.pixel_shader_source, "PSMain", "ps_5_0");
    if (pixel_bytecode.is_error()) {
        return core::Result<RenderedTexture, PreviewError>::failure(pixel_bytecode.error());
    }
    ComPtr<ID3D11PixelShader> pixel;
    HRESULT hr = device->CreatePixelShader(
        pixel_bytecode.value()->GetBufferPointer(),
        pixel_bytecode.value()->GetBufferSize(),
        nullptr,
        pixel.GetAddressOf());
    if (FAILED(hr)) {
        return core::Result<RenderedTexture, PreviewError>::failure(make_error(hresult_text("CreatePixelShader", hr)));
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = recipe.render.width;
    texture_desc.Height = recipe.render.height;
    texture_desc.MipLevels = 1U;
    texture_desc.ArraySize = 1U;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1U;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    RenderedTexture rendered;
    hr = device->CreateTexture2D(&texture_desc, nullptr, rendered.texture.GetAddressOf());
    if (FAILED(hr)) {
        return core::Result<RenderedTexture, PreviewError>::failure(make_error(hresult_text("CreateTexture2D", hr)));
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device->CreateRenderTargetView(rendered.texture.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr)) {
        return core::Result<RenderedTexture, PreviewError>::failure(make_error(hresult_text("CreateRenderTargetView", hr)));
    }
    hr = device->CreateShaderResourceView(rendered.texture.Get(), nullptr, rendered.srv.GetAddressOf());
    if (FAILED(hr)) {
        return core::Result<RenderedTexture, PreviewError>::failure(make_error(hresult_text("CreateShaderResourceView", hr)));
    }

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(recipe.render.width);
    viewport.Height = static_cast<float>(recipe.render.height);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;
    context->OMSetRenderTargets(1U, rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(1U, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.value().Get(), nullptr, 0U);
    context->PSSetShader(pixel.Get(), nullptr, 0U);
    context->Draw(3U, 0U);
    ID3D11RenderTargetView* null_rtv = nullptr;
    context->OMSetRenderTargets(1U, &null_rtv, nullptr);
    return core::Result<RenderedTexture, PreviewError>::success(std::move(rendered));
}

[[nodiscard]] core::Result<nodes::Image, PreviewError> readback_texture(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    const u32 width,
    const u32 height) {
    D3D11_TEXTURE2D_DESC staging_desc{};
    source->GetDesc(&staging_desc);
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0U;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0U;
    ComPtr<ID3D11Texture2D> staging;
    const HRESULT create_hr = device->CreateTexture2D(&staging_desc, nullptr, staging.GetAddressOf());
    if (FAILED(create_hr)) {
        return core::Result<nodes::Image, PreviewError>::failure(make_error(hresult_text("CreateTexture2D(staging)", create_hr)));
    }
    context->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map_hr = context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped);
    if (FAILED(map_hr)) {
        return core::Result<nodes::Image, PreviewError>::failure(make_error(hresult_text("Map(staging)", map_hr)));
    }

    nodes::Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    const auto* source_bytes = static_cast<const u8*>(mapped.pData);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
    for (u32 y = 0U; y < height; ++y) {
        std::copy_n(
            source_bytes + static_cast<std::size_t>(mapped.RowPitch) * static_cast<std::size_t>(y),
            row_bytes,
            image.rgba.data() + row_bytes * static_cast<std::size_t>(y));
    }
    context->Unmap(staging.Get(), 0U);
    return core::Result<nodes::Image, PreviewError>::success(std::move(image));
}

[[nodiscard]] core::Result<RenderedTexture, PreviewError> upload_cpu_image(
    ID3D11Device* device,
    const nodes::Image& image) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = 1U;
    desc.ArraySize = 1U;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1U;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = image.rgba.data();
    data.SysMemPitch = image.width * 4U;

    RenderedTexture rendered;
    HRESULT hr = device->CreateTexture2D(&desc, &data, rendered.texture.GetAddressOf());
    if (FAILED(hr)) {
        return core::Result<RenderedTexture, PreviewError>::failure(make_error(hresult_text("CreateTexture2D(CPU preview)", hr)));
    }
    hr = device->CreateShaderResourceView(rendered.texture.Get(), nullptr, rendered.srv.GetAddressOf());
    if (FAILED(hr)) {
        return core::Result<RenderedTexture, PreviewError>::failure(make_error(hresult_text("CreateShaderResourceView(CPU preview)", hr)));
    }
    return core::Result<RenderedTexture, PreviewError>::success(std::move(rendered));
}

[[nodiscard]] std::string fallback_message(const PreviewPlan& plan) {
    std::string message = "canonical CPU fallback";
    if (!plan.unsupported_nodes.empty()) {
        message += ": unsupported GPU node(s): ";
        for (std::size_t index = 0U; index < plan.unsupported_nodes.size(); ++index) {
            if (index != 0U) {
                message += ", ";
            }
            message += plan.unsupported_nodes[index];
        }
    }
    return message;
}

}  // namespace

PreviewPlan build_preview_plan(const core::Recipe& recipe) {
    return ShaderPlanBuilder(recipe).build();
}

core::Result<nodes::Image, PreviewError> render_preview_warp(
    const core::Recipe& recipe,
    PreviewStatus* status) {
    const auto validation = core::validate_recipe(recipe);
    if (!validation.empty()) {
        return core::Result<nodes::Image, PreviewError>::failure(make_error(
            "GPU preview requires a graph-valid recipe: " + validation.front().message));
    }

    const PreviewPlan plan = build_preview_plan(recipe);
    if (!plan.gpu_supported) {
        auto canonical = nodes::render_reference(recipe);
        if (canonical.is_error()) {
            return core::Result<nodes::Image, PreviewError>::failure(make_error(
                "canonical fallback failed: " + canonical.error().message));
        }
        if (status != nullptr) {
            *status = PreviewStatus{PreviewPath::canonical_cpu_fallback, fallback_message(plan), plan.unsupported_nodes};
        }
        return core::Result<nodes::Image, PreviewError>::success(std::move(canonical).value());
    }

    auto device = create_offscreen_device(true);
    if (device.is_error()) {
        return core::Result<nodes::Image, PreviewError>::failure(device.error());
    }
    auto rendered = render_plan_to_texture(
        device.value().device.Get(),
        device.value().context.Get(),
        recipe,
        plan);
    if (rendered.is_error()) {
        return core::Result<nodes::Image, PreviewError>::failure(rendered.error());
    }
    auto image = readback_texture(
        device.value().device.Get(),
        device.value().context.Get(),
        rendered.value().texture.Get(),
        recipe.render.width,
        recipe.render.height);
    if (image.is_error()) {
        return image;
    }
    if (status != nullptr) {
        *status = PreviewStatus{PreviewPath::gpu, "GPU preview (D3D11 WARP)", {}};
    }
    return image;
}

EquivalenceStats compare_images(const nodes::Image& canonical, const nodes::Image& preview) {
    EquivalenceStats stats;
    if (canonical.width != preview.width || canonical.height != preview.height || canonical.rgba.size() != preview.rgba.size()) {
        stats.maximum_channel_error = 255U;
        stats.mean_absolute_channel_error = 255.0;
        stats.differing_channels = (std::max)(canonical.rgba.size(), preview.rgba.size());
        return stats;
    }
    std::uint64_t total_error = 0U;
    for (std::size_t index = 0U; index < canonical.rgba.size(); ++index) {
        const int error = std::abs(static_cast<int>(canonical.rgba[index]) - static_cast<int>(preview.rgba[index]));
        stats.maximum_channel_error = (std::max)(stats.maximum_channel_error, static_cast<u8>(error));
        total_error += static_cast<std::uint64_t>(error);
        if (error != 0) {
            ++stats.differing_channels;
        }
    }
    if (!canonical.rgba.empty()) {
        stats.mean_absolute_channel_error = static_cast<double>(total_error) / static_cast<double>(canonical.rgba.size());
    }
    return stats;
}

struct D3d11Preview::Impl final {
    HWND window{nullptr};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> backbuffer_rtv;
    ComPtr<ID3D11VertexShader> fullscreen_vs;
    ComPtr<ID3D11PixelShader> copy_ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> preview_texture;
    ComPtr<ID3D11ShaderResourceView> preview_srv;
    std::optional<core::Recipe> recipe;
    PreviewStatus status;
    PreviewDiagnostics diagnostics;
    u32 width{0U};
    u32 height{0U};

    [[nodiscard]] core::Result<void, PreviewError> create_backbuffer() {
        ComPtr<ID3D11Texture2D> backbuffer;
        HRESULT hr = swap_chain->GetBuffer(0U, IID_PPV_ARGS(backbuffer.GetAddressOf()));
        if (FAILED(hr)) {
            return core::Result<void, PreviewError>::failure(make_error(hresult_text("IDXGISwapChain::GetBuffer", hr)));
        }
        hr = device->CreateRenderTargetView(backbuffer.Get(), nullptr, backbuffer_rtv.GetAddressOf());
        if (FAILED(hr)) {
            return core::Result<void, PreviewError>::failure(make_error(hresult_text("CreateRenderTargetView(backbuffer)", hr)));
        }
        return core::Result<void, PreviewError>::success();
    }

    [[nodiscard]] core::Result<void, PreviewError> create_present_pipeline() {
        auto vertex_bytecode = compile_shader(kFullscreenShader, "VSMain", "vs_5_0");
        if (vertex_bytecode.is_error()) {
            return core::Result<void, PreviewError>::failure(vertex_bytecode.error());
        }
        HRESULT hr = device->CreateVertexShader(
            vertex_bytecode.value()->GetBufferPointer(),
            vertex_bytecode.value()->GetBufferSize(),
            nullptr,
            fullscreen_vs.GetAddressOf());
        if (FAILED(hr)) {
            return core::Result<void, PreviewError>::failure(make_error(hresult_text("CreateVertexShader(copy)", hr)));
        }
        auto pixel_bytecode = compile_shader(kFullscreenShader, "PSCopy", "ps_5_0");
        if (pixel_bytecode.is_error()) {
            return core::Result<void, PreviewError>::failure(pixel_bytecode.error());
        }
        hr = device->CreatePixelShader(
            pixel_bytecode.value()->GetBufferPointer(),
            pixel_bytecode.value()->GetBufferSize(),
            nullptr,
            copy_ps.GetAddressOf());
        if (FAILED(hr)) {
            return core::Result<void, PreviewError>::failure(make_error(hresult_text("CreatePixelShader(copy)", hr)));
        }

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sampler_desc, sampler.GetAddressOf());
        if (FAILED(hr)) {
            return core::Result<void, PreviewError>::failure(make_error(hresult_text("CreateSamplerState", hr)));
        }
        return core::Result<void, PreviewError>::success();
    }

    [[nodiscard]] core::Result<void, PreviewError> create_device_swap_chain() {
        RECT client{};
        if (GetClientRect(window, &client) == 0) {
            return core::Result<void, PreviewError>::failure(make_error("GetClientRect failed while creating D3D11 preview"));
        }
        width = static_cast<u32>((std::max)(client.right - client.left, 1L));
        height = static_cast<u32>((std::max)(client.bottom - client.top, 1L));

        DXGI_SWAP_CHAIN_DESC swap_desc{};
        swap_desc.BufferDesc.Width = width;
        swap_desc.BufferDesc.Height = height;
        swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_desc.SampleDesc.Count = 1U;
        swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_desc.BufferCount = 2U;
        swap_desc.OutputWindow = window;
        swap_desc.Windowed = TRUE;
        swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            1U,
            D3D11_SDK_VERSION,
            &swap_desc,
            swap_chain.GetAddressOf(),
            device.GetAddressOf(),
            &selected,
            context.GetAddressOf());
        if (FAILED(hr)) {
            diagnostics.warp_device = true;
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                1U,
                D3D11_SDK_VERSION,
                &swap_desc,
                swap_chain.ReleaseAndGetAddressOf(),
                device.ReleaseAndGetAddressOf(),
                &selected,
                context.ReleaseAndGetAddressOf());
        } else {
            diagnostics.warp_device = false;
        }
        if (FAILED(hr)) {
            return core::Result<void, PreviewError>::failure(make_error(hresult_text("D3D11CreateDeviceAndSwapChain", hr)));
        }
        auto backbuffer = create_backbuffer();
        if (backbuffer.is_error()) {
            return backbuffer;
        }
        return create_present_pipeline();
    }

    void release_device_resources() {
        if (context != nullptr) {
            context->ClearState();
        }
        preview_srv.Reset();
        preview_texture.Reset();
        sampler.Reset();
        copy_ps.Reset();
        fullscreen_vs.Reset();
        backbuffer_rtv.Reset();
        swap_chain.Reset();
        context.Reset();
        device.Reset();
    }

    [[nodiscard]] core::Result<PreviewStatus, PreviewError> rebuild_recipe_texture() {
        if (!recipe.has_value()) {
            return core::Result<PreviewStatus, PreviewError>::success(status);
        }
        const PreviewPlan plan = build_preview_plan(*recipe);
        if (plan.gpu_supported) {
            auto rendered = render_plan_to_texture(device.Get(), context.Get(), *recipe, plan);
            if (rendered.is_ok()) {
                preview_texture = std::move(rendered.value().texture);
                preview_srv = std::move(rendered.value().srv);
                status = PreviewStatus{PreviewPath::gpu, diagnostics.warp_device ? "GPU preview (D3D11 WARP)" : "GPU preview (D3D11 hardware)", {}};
                return core::Result<PreviewStatus, PreviewError>::success(status);
            }
            auto canonical = nodes::render_reference(*recipe);
            if (canonical.is_error()) {
                return core::Result<PreviewStatus, PreviewError>::failure(make_error(
                    "GPU preview failed (" + rendered.error().message + ") and canonical fallback failed: " + canonical.error().message));
            }
            auto uploaded = upload_cpu_image(device.Get(), canonical.value());
            if (uploaded.is_error()) {
                return core::Result<PreviewStatus, PreviewError>::failure(uploaded.error());
            }
            preview_texture = std::move(uploaded.value().texture);
            preview_srv = std::move(uploaded.value().srv);
            status = PreviewStatus{
                PreviewPath::canonical_cpu_fallback,
                "canonical CPU fallback after GPU shader failure: " + rendered.error().message,
                {},
            };
            return core::Result<PreviewStatus, PreviewError>::success(status);
        }

        auto canonical = nodes::render_reference(*recipe);
        if (canonical.is_error()) {
            return core::Result<PreviewStatus, PreviewError>::failure(make_error(
                "canonical CPU fallback failed: " + canonical.error().message));
        }
        auto uploaded = upload_cpu_image(device.Get(), canonical.value());
        if (uploaded.is_error()) {
            return core::Result<PreviewStatus, PreviewError>::failure(uploaded.error());
        }
        preview_texture = std::move(uploaded.value().texture);
        preview_srv = std::move(uploaded.value().srv);
        status = PreviewStatus{PreviewPath::canonical_cpu_fallback, fallback_message(plan), plan.unsupported_nodes};
        return core::Result<PreviewStatus, PreviewError>::success(status);
    }

    [[nodiscard]] core::Result<void, PreviewError> recreate_after_device_loss() {
        release_device_resources();
        auto created = create_device_swap_chain();
        if (created.is_error()) {
            return created;
        }
        auto rebuilt = rebuild_recipe_texture();
        if (rebuilt.is_error()) {
            return core::Result<void, PreviewError>::failure(rebuilt.error());
        }
        return core::Result<void, PreviewError>::success();
    }
};

D3d11Preview::D3d11Preview() : impl_(std::make_unique<Impl>()) {}
D3d11Preview::~D3d11Preview() = default;
D3d11Preview::D3d11Preview(D3d11Preview&&) noexcept = default;
D3d11Preview& D3d11Preview::operator=(D3d11Preview&&) noexcept = default;

core::Result<void, PreviewError> D3d11Preview::initialize(HWND window) {
    if (window == nullptr) {
        return core::Result<void, PreviewError>::failure(make_error("cannot initialize D3D11 preview with a null HWND"));
    }
    impl_->window = window;
    return impl_->create_device_swap_chain();
}

core::Result<PreviewStatus, PreviewError> D3d11Preview::set_recipe(const core::Recipe& recipe) {
    if (impl_->device == nullptr) {
        return core::Result<PreviewStatus, PreviewError>::failure(make_error("D3D11 preview is not initialized"));
    }
    const auto validation = core::validate_recipe(recipe);
    if (!validation.empty()) {
        return core::Result<PreviewStatus, PreviewError>::failure(make_error(
            "preview requires a graph-valid recipe: " + validation.front().message));
    }
    impl_->recipe = recipe;
    return impl_->rebuild_recipe_texture();
}

core::Result<void, PreviewError> D3d11Preview::resize(const u32 width, const u32 height) {
    if (impl_->swap_chain == nullptr || width == 0U || height == 0U) {
        return core::Result<void, PreviewError>::success();
    }
    if (width == impl_->width && height == impl_->height) {
        return core::Result<void, PreviewError>::success();
    }
    impl_->backbuffer_rtv.Reset();
    const HRESULT hr = impl_->swap_chain->ResizeBuffers(0U, width, height, DXGI_FORMAT_UNKNOWN, 0U);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        return impl_->recreate_after_device_loss();
    }
    if (FAILED(hr)) {
        return core::Result<void, PreviewError>::failure(make_error(hresult_text("IDXGISwapChain::ResizeBuffers", hr)));
    }
    impl_->width = width;
    impl_->height = height;
    return impl_->create_backbuffer();
}

core::Result<void, PreviewError> D3d11Preview::present() {
    if (impl_->swap_chain == nullptr || impl_->backbuffer_rtv == nullptr) {
        return core::Result<void, PreviewError>::failure(make_error("D3D11 preview is not initialized"));
    }
    if (impl_->width == 0U || impl_->height == 0U) {
        return core::Result<void, PreviewError>::success();
    }

    const auto started = std::chrono::steady_clock::now();
    constexpr float clear_colour[]{0.025F, 0.028F, 0.035F, 1.0F};
    impl_->context->OMSetRenderTargets(1U, impl_->backbuffer_rtv.GetAddressOf(), nullptr);
    impl_->context->ClearRenderTargetView(impl_->backbuffer_rtv.Get(), clear_colour);

    if (impl_->preview_srv != nullptr) {
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(impl_->width);
        viewport.Height = static_cast<float>(impl_->height);
        viewport.MinDepth = 0.0F;
        viewport.MaxDepth = 1.0F;
        impl_->context->RSSetViewports(1U, &viewport);
        impl_->context->IASetInputLayout(nullptr);
        impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->context->VSSetShader(impl_->fullscreen_vs.Get(), nullptr, 0U);
        impl_->context->PSSetShader(impl_->copy_ps.Get(), nullptr, 0U);
        impl_->context->PSSetShaderResources(0U, 1U, impl_->preview_srv.GetAddressOf());
        impl_->context->PSSetSamplers(0U, 1U, impl_->sampler.GetAddressOf());
        impl_->context->Draw(3U, 0U);
        ID3D11ShaderResourceView* null_srv = nullptr;
        impl_->context->PSSetShaderResources(0U, 1U, &null_srv);
    }

    const HRESULT hr = impl_->swap_chain->Present(1U, 0U);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        return impl_->recreate_after_device_loss();
    }
    if (FAILED(hr)) {
        return core::Result<void, PreviewError>::failure(make_error(hresult_text("IDXGISwapChain::Present", hr)));
    }

    const auto finished = std::chrono::steady_clock::now();
    impl_->diagnostics.last_present_milliseconds =
        std::chrono::duration<double, std::milli>(finished - started).count();
    ++impl_->diagnostics.presented_frames;
    return core::Result<void, PreviewError>::success();
}

PreviewDiagnostics D3d11Preview::diagnostics() const noexcept {
    return impl_->diagnostics;
}

bool D3d11Preview::initialized() const noexcept {
    return impl_->device != nullptr && impl_->swap_chain != nullptr;
}

}  // namespace artminer::gpu
