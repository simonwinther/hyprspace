#include "PreviewFilter.hpp"

#include "Capture.hpp"
#include "Config.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <drm_fourcc.h>
#include <optional>
#include <unordered_map>

namespace hyprspace {
    namespace {
        // A separate sampler pass leaves the compositor's surface shader and
        // feature variants untouched. Texture coordinates are copied upright;
        // monitor transforms are applied once by the final native draw.
        constexpr auto VERTEX   = R"(#version 300 es
precision highp float;
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
        constexpr auto FRAGMENT = R"(#version 300 es
precision highp float;
in vec2 uv;
uniform sampler2D source;
uniform vec2 destinationSize;
uniform int stage;
uniform int sourceTransfer;
out vec4 color;

vec3 decodeRGB(vec3 rgb) {
    if (sourceTransfer == 1)
        return pow(max(rgb, vec3(0.0)), vec3(2.2));
    if (sourceTransfer == 2)
        return pow(max(rgb, vec3(0.0)), vec3(2.8));
    if (sourceTransfer == 3) {
        bvec3 low = lessThanEqual(rgb, vec3(0.04045));
        return mix(pow((max(rgb, vec3(0.0)) + 0.055) / 1.055, vec3(2.4)), rgb / 12.92, low);
    }
    return rgb;
}

vec3 encodeRGB(vec3 rgb) {
    rgb = max(rgb, vec3(0.0));
    if (sourceTransfer == 1)
        return pow(rgb, vec3(1.0 / 2.2));
    if (sourceTransfer == 2)
        return pow(rgb, vec3(1.0 / 2.8));
    if (sourceTransfer == 3) {
        bvec3 low = lessThanEqual(rgb, vec3(0.0031308));
        return mix(1.055 * pow(rgb, vec3(1.0 / 2.4)) - 0.055, 12.92 * rgb, low);
    }
    return rgb;
}

// Captures and the final native draw use premultiplied colour. Decode each
// unassociated texel before interpolation, then associate it again in linear
// light. Decoding a hardware-bilinear result is already too late: a thin white
// stroke mixed with its dark background has lost light energy by that point.
vec4 decodePixel(vec4 pixel) {
    if (sourceTransfer == 0)
        return pixel;
    if (pixel.a <= 0.0)
        return vec4(0.0);
    return vec4(decodeRGB(pixel.rgb / pixel.a) * pixel.a, pixel.a);
}

vec4 encodePixel(vec4 pixel) {
    if (sourceTransfer == 0)
        return pixel;
    if (pixel.a <= 0.0)
        return vec4(0.0);
    return vec4(clamp(encodeRGB(pixel.rgb / pixel.a), vec3(0.0), vec3(1.0)) * pixel.a, pixel.a);
}

vec4 linearTexel(ivec2 point, ivec2 size) {
    return decodePixel(texelFetch(source, clamp(point, ivec2(0), size - 1), 0));
}

vec3 unassociated(vec4 pixel, vec3 transparentFallback) {
    return pixel.a > 0.0 ? pixel.rgb / pixel.a : transparentFallback;
}

vec4 linearSample(vec2 point, ivec2 size) {
    if (sourceTransfer == 0)
        return texture(source, point);
    vec2 position = point * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(position));
    vec2 fraction = fract(position);
    return mix(mix(linearTexel(base, size), linearTexel(base + ivec2(1, 0), size), fraction.x),
               mix(linearTexel(base + ivec2(0, 1), size), linearTexel(base + ivec2(1, 1), size), fraction.x), fraction.y);
}

void main() {
    ivec2 size = textureSize(source, 0);
    vec2 sourceSize = vec2(size);
    if (stage == 0) {
        color = encodePixel(linearSample(uv, size));
    } else if (stage == 1) {
        vec2 footprint = max(sourceSize / destinationSize, vec2(1.0));
        vec4 sum = vec4(0.0);
        if (all(lessThanEqual(footprint, vec2(8.0)))) {
            // Integrate each intersecting source pixel exactly once. Normal
            // overview sizes usually cover only 16-36 unique texels, whereas
            // stratified bilinear taps decode many of those texels repeatedly.
            // An eight-pixel footprint intersects at most nine pixel cells.
            vec2 firstEdge = uv * sourceSize - footprint * 0.5;
            vec2 lastEdge = firstEdge + footprint;
            ivec2 first = ivec2(floor(firstEdge));
            ivec2 last = ivec2(ceil(lastEdge));
            for (int y = 0; y < 9; ++y) {
                int sourceY = first.y + y;
                if (sourceY >= last.y)
                    break;
                float weightY = max(0.0, min(lastEdge.y, float(sourceY + 1)) - max(firstEdge.y, float(sourceY)));
                for (int x = 0; x < 9; ++x) {
                    int sourceX = first.x + x;
                    if (sourceX >= last.x)
                        break;
                    float weightX = max(0.0, min(lastEdge.x, float(sourceX + 1)) - max(firstEdge.x, float(sourceX)));
                    sum += linearTexel(ivec2(sourceX, sourceY), size) * (weightX * weightY);
                }
            }
            color = encodePixel(sum / (footprint.x * footprint.y));
        } else {
            // Very small cards keep the fixed 16-sample footprint estimate,
            // bounding source fetches even for an arbitrarily large desktop.
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    vec2 offset = (vec2(float(x), float(y)) + 0.5) / 4.0 - 0.5;
                    sum += linearSample(uv + offset * footprint / sourceSize, size);
                }
            color = encodePixel(sum / 16.0);
        }
    } else {
        ivec2 point = ivec2(uv * sourceSize);
        vec4 c = linearTexel(point, size);
        if (c.a <= 0.0) {
            color = vec4(0.0);
            return;
        }
        vec3 centre = c.rgb / c.a;
        vec3 n = unassociated(linearTexel(point + ivec2(0, 1), size), centre);
        vec3 s = unassociated(linearTexel(point - ivec2(0, 1), size), centre);
        vec3 e = unassociated(linearTexel(point + ivec2(1, 0), size), centre);
        vec3 w = unassociated(linearTexel(point - ivec2(1, 0), size), centre);
        vec3 lo = min(centre, min(min(n, s), min(e, w)));
        vec3 hi = max(centre, max(max(n, s), max(e, w)));
        // Sharpen unassociated colour and preserve centre coverage. Treating
        // differing alpha as colour contrast creates a fringe at transparent
        // edges. Keep extended linear values rather than clamp HDR to alpha.
        vec3 rgb = clamp(centre + 0.22 * (centre - (n + s + e + w) * 0.25), lo, hi);
        color = encodePixel(vec4(rgb * c.a, c.a));
    }
}
)";

        // Hyprland caches several GL values. Save and restore actual state
        // without changing its caches, so subsequent native draws see exactly
        // the state they left behind before this self-contained pass.
        struct SGLState {
            GLint     draw = 0, read = 0, program = 0, vao = 0, active = 0, texture = 0, sampler = 0;
            GLint     viewport[4]{};
            GLboolean blend = false, scissor = false, stencil = false, depth = false, cull = false, write[4]{};
            SGLState() {
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
                glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
                glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
                glGetIntegerv(GL_VIEWPORT, viewport);
                blend   = glIsEnabled(GL_BLEND);
                scissor = glIsEnabled(GL_SCISSOR_TEST);
                stencil = glIsEnabled(GL_STENCIL_TEST);
                depth   = glIsEnabled(GL_DEPTH_TEST);
                cull    = glIsEnabled(GL_CULL_FACE);
                glGetBooleanv(GL_COLOR_WRITEMASK, write);
                glActiveTexture(GL_TEXTURE0);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
                glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
            }
            ~SGLState() {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
                glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
                // Releasing/reallocating a native framebuffer can update the
                // backend's viewport cache as it rebinds the current target.
                if (const auto GL = g_pHyprRenderer->glBackend())
                    GL->setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
                glUseProgram(program);
                glBindVertexArray(vao);
                glBindTexture(GL_TEXTURE_2D, texture);
                glBindSampler(0, sampler);
                glActiveTexture(active);
                if (blend)
                    glEnable(GL_BLEND);
                else
                    glDisable(GL_BLEND);
                if (scissor)
                    glEnable(GL_SCISSOR_TEST);
                else
                    glDisable(GL_SCISSOR_TEST);
                if (stencil)
                    glEnable(GL_STENCIL_TEST);
                else
                    glDisable(GL_STENCIL_TEST);
                if (depth)
                    glEnable(GL_DEPTH_TEST);
                else
                    glDisable(GL_DEPTH_TEST);
                if (cull)
                    glEnable(GL_CULL_FACE);
                else
                    glDisable(GL_CULL_FACE);
                glColorMask(write[0], write[1], write[2], write[3]);
            }
        };

        struct SEntry {
            WP<Render::ITexture>                  source;
            SP<Render::IFramebuffer>              area, sharp;
            uint64_t                              revision = 0, renderedRevision = 0;
            std::string                           renderedMode;
            int                                   renderedTransfer = -1;
            std::chrono::steady_clock::time_point retryAfter{};
        };
        std::unordered_map<Render::ITexture*, SEntry> entries;
        SPreviewFilterResources                       resources;
        GLuint                                        program = 0, vao = 0, sampler = 0;
        GLint                                         sourceUniform = -1, destinationUniform = -1, stageUniform = -1, transferUniform = -1;
        bool                                          shaderFailed = false;

        GLuint compile(GLenum type, const char* text) {
            const GLuint shader = glCreateShader(type);
            if (!shader)
                return 0;
            glShaderSource(shader, 1, &text, nullptr);
            glCompileShader(shader);
            GLint compiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled)
                return shader;
            glDeleteShader(shader);
            return 0;
        }

        bool prepareShader() {
            if (program && vao && sampler)
                return true;
            if (shaderFailed)
                return false;
            const GLuint vertex = compile(GL_VERTEX_SHADER, VERTEX), fragment = compile(GL_FRAGMENT_SHADER, FRAGMENT);
            if (vertex && fragment) {
                program = glCreateProgram();
                glAttachShader(program, vertex);
                glAttachShader(program, fragment);
                glLinkProgram(program);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (!linked) {
                    glDeleteProgram(program);
                    program = 0;
                }
            }
            if (vertex)
                glDeleteShader(vertex);
            if (fragment)
                glDeleteShader(fragment);
            if (program) {
                glGenVertexArrays(1, &vao);
                glGenSamplers(1, &sampler);
                glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                sourceUniform      = glGetUniformLocation(program, "source");
                destinationUniform = glGetUniformLocation(program, "destinationSize");
                stageUniform       = glGetUniformLocation(program, "stage");
                transferUniform    = glGetUniformLocation(program, "sourceTransfer");
            }
            if (!program || !vao || !sampler) {
                shaderFailed = true;
                ++resources.failures;
                return false;
            }
            return true;
        }

        bool framebuffer(SP<Render::IFramebuffer>& fb, int width, int height, const SP<Render::ITexture>& source) {
            if (!fb || fb->m_size != Vector2D{width, height}) {
                fb.reset();
                try {
                    fb = createCaptureFramebuffer();
                    if (!fb || !fb->alloc(width, height, DRM_FORMAT_ABGR8888) || !fb->isAllocated()) {
                        fb.reset();
                        return false;
                    }
                    ++resources.allocations;
                } catch (const std::bad_alloc&) {
                    fb.reset();
                    return false;
                }
            }
            fb->setImageDescription(source->m_imageDescription);
            fb->getTexture()->m_transform = source->m_transform;
            return true;
        }

        void render(const SP<Render::ITexture>& source, const SP<Render::IFramebuffer>& target, int stage, int transfer) {
            // bind() also adjusts Hyprland's viewport cache. This pass owns its
            // viewport locally, so bind just the framebuffer name instead.
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<Render::GL::CGLFramebuffer*>(target.get())->getFBID());
            glViewport(0, 0, target->m_size.x, target->m_size.y);
            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glUseProgram(program);
            glBindVertexArray(vao);
            glActiveTexture(GL_TEXTURE0);
            source->bind();
            // A private sampler avoids mutating the captured texture's state
            // or Hyprland's per-texture parameter cache.
            glBindSampler(0, sampler);
            glUniform1i(sourceUniform, 0);
            glUniform2f(destinationUniform, target->m_size.x, target->m_size.y);
            glUniform1i(stageUniform, stage);
            glUniform1i(transferUniform, transfer);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            ++resources.passes;
        }

        void account() {
            resources.entries = entries.size();
            resources.bytes   = 0;
            for (const auto& [_, entry] : entries) {
                for (const auto& fb : {entry.area, entry.sharp})
                    if (fb)
                        resources.bytes += static_cast<size_t>(fb->m_size.x) * static_cast<size_t>(fb->m_size.y) * 4;
            }
            resources.peakBytes = std::max(resources.peakBytes, resources.bytes);
        }

        std::optional<int> sourceTransfer(const SP<Render::ITexture>& source) {
            // Image descriptions belong to the compositor's unique registry;
            // borrow their weak handles without promoting them with lock().
            const auto& description = source->m_imageDescription;
            if (!description)
                return std::nullopt;
            using namespace NColorManagement;
            switch (description->value().transferFunction) {
            case CM_TRANSFER_FUNCTION_LINEAR:
            case CM_TRANSFER_FUNCTION_EXT_LINEAR:
                return 0;
            case CM_TRANSFER_FUNCTION_GAMMA22:
                return 1;
            case CM_TRANSFER_FUNCTION_GAMMA28:
                return 2;
            case CM_TRANSFER_FUNCTION_SRGB:
            case CM_TRANSFER_FUNCTION_EXT_SRGB:
                return 3;
            default:
                // HDR and other transfer functions need their own luminance
                // range handling. Preserve the native path rather than guess.
                return std::nullopt;
            }
        }
    } // namespace

    SP<Render::ITexture> previewFilteredTexture(const SP<Render::ITexture>& source, const Vector2D& destination, bool stableSize) {
        const auto mode = config::overviewPreviewFilter();
        if (!stableSize) {
            ++resources.animatedBypasses;
            return source;
        }
        if (!source || mode == "linear" || !std::isfinite(destination.x) || !std::isfinite(destination.y) || destination.x <= 0 || destination.y <= 0 ||
            source->m_size.x <= 0 || source->m_size.y <= 0)
            return source;
        // Magnified and nearly one-to-one previews retain their original
        // samples, including the desktop endpoints of opening/closing motion.
        const int width  = static_cast<int>(std::clamp(std::ceil(destination.x), 1.0, source->m_size.x));
        const int height = static_cast<int>(std::clamp(std::ceil(destination.y), 1.0, source->m_size.y));
        if (width >= source->m_size.x * 0.95 && height >= source->m_size.y * 0.95)
            return source;
        const auto transfer = sourceTransfer(source);
        if (!transfer)
            return source;
        const bool sharpen = mode == "sharp" || mode == "linear-sharp";
        auto&      entry   = entries[source.get()];
        if (entry.source.lock() != source) {
            entry        = {};
            entry.source = source;
        }
        if (std::chrono::steady_clock::now() < entry.retryAfter)
            return source;
        if (entry.area && entry.area->m_size == Vector2D{width, height} && entry.renderedRevision == entry.revision && entry.renderedMode == mode &&
            entry.renderedTransfer == *transfer &&
            (!sharpen || entry.sharp)) {
            ++resources.hits;
            return sharpen ? entry.sharp->getTexture() : entry.area->getTexture();
        }
        const SGLState state;
        if (!prepareShader() || !framebuffer(entry.area, width, height, source) || (sharpen && !framebuffer(entry.sharp, width, height, source))) {
            ++resources.failures;
            entry.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            account();
            return source;
        }
        render(source, entry.area, mode == "linear-sharp" ? 0 : 1, *transfer);
        if (sharpen)
            render(entry.area->getTexture(), entry.sharp, 2, *transfer);
        else
            entry.sharp.reset();
        entry.renderedRevision = entry.revision;
        entry.renderedMode     = mode;
        entry.renderedTransfer = *transfer;
        account();
        return sharpen ? entry.sharp->getTexture() : entry.area->getTexture();
    }

    void previewFilterCaptured(const SP<Render::ITexture>& source) {
        const auto it = source ? entries.find(source.get()) : entries.end();
        if (it != entries.end() && it->second.source.lock() == source)
            ++it->second.revision;
    }

    void prunePreviewFilters() {
        std::erase_if(entries, [](const auto& entry) { return entry.second.source.expired(); });
        account();
    }

    void clearPreviewFilters() {
        entries.clear();
        account();
    }

    void destroyPreviewFilters() {
        clearPreviewFilters();
        if (program)
            glDeleteProgram(program);
        if (vao)
            glDeleteVertexArrays(1, &vao);
        if (sampler)
            glDeleteSamplers(1, &sampler);
        program = vao = sampler = 0;
        shaderFailed            = false;
    }

    const SPreviewFilterResources& previewFilterResources() {
        prunePreviewFilters();
        return resources;
    }
} // namespace hyprspace
