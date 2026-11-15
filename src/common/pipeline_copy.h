#pragma once

// A deep copy of a ReShade pipeline description. It holds the sub-objects and
// everything their pointers point at. A pipeline can be made again from it
// with one shader swapped, even after the game's own description is gone.

#include <reshade.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace pipeline_copy
{
using namespace reshade::api;

struct stored_desc
{
    std::vector<pipeline_subobject> subs;
    std::deque<std::vector<uint8_t>> blobs;
    std::deque<shader_desc> shaders;
    std::deque<std::vector<input_element>> inputs;
    std::deque<std::string> names;
    size_t pixel_shader = SIZE_MAX;   // index in subs
    size_t compute_shader = SIZE_MAX; // index in subs
    uint32_t unknown_type = 0;      // the sub-object type that stopped the copy

    void *keep(const void *data, size_t size)
    {
        blobs.emplace_back(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
        return blobs.back().data();
    }

    bool copy(uint32_t count, const pipeline_subobject *from)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            pipeline_subobject s = from[i];
            if (s.data == nullptr || s.count == 0)
            {
                subs.push_back(s);
                continue;
            }
            switch (s.type)
            {
            case pipeline_subobject_type::vertex_shader:
            case pipeline_subobject_type::hull_shader:
            case pipeline_subobject_type::domain_shader:
            case pipeline_subobject_type::geometry_shader:
            case pipeline_subobject_type::pixel_shader:
            case pipeline_subobject_type::compute_shader: {
                shader_desc d = *static_cast<const shader_desc *>(s.data);
                if (d.spec_constants != 0)
                {
                    unknown_type = uint32_t(s.type);
                    return false;
                }
                d.code = keep(d.code, d.code_size);
                if (d.entry_point != nullptr)
                {
                    names.emplace_back(d.entry_point);
                    d.entry_point = names.back().c_str();
                }
                shaders.push_back(d);
                s.data = &shaders.back();
                if (s.type == pipeline_subobject_type::pixel_shader)
                    pixel_shader = subs.size();
                if (s.type == pipeline_subobject_type::compute_shader)
                    compute_shader = subs.size();
                break;
            }
            case pipeline_subobject_type::input_layout: {
                const auto *e = static_cast<const input_element *>(s.data);
                inputs.emplace_back(e, e + s.count);
                for (input_element &el : inputs.back())
                    if (el.semantic != nullptr)
                    {
                        names.emplace_back(el.semantic);
                        el.semantic = names.back().c_str();
                    }
                s.data = inputs.back().data();
                break;
            }
            case pipeline_subobject_type::stream_output_state:
                // ReShade always passes one for D3D12 graphics pipelines.
                s.data = keep(s.data, sizeof(stream_output_desc));
                break;
            case pipeline_subobject_type::blend_state:
                s.data = keep(s.data, sizeof(blend_desc));
                break;
            case pipeline_subobject_type::rasterizer_state:
                s.data = keep(s.data, sizeof(rasterizer_desc));
                break;
            case pipeline_subobject_type::depth_stencil_state:
                s.data = keep(s.data, sizeof(depth_stencil_desc));
                break;
            case pipeline_subobject_type::primitive_topology:
                s.data = keep(s.data, sizeof(primitive_topology));
                break;
            case pipeline_subobject_type::depth_stencil_format:
                s.data = keep(s.data, sizeof(format));
                break;
            case pipeline_subobject_type::render_target_formats:
                s.data = keep(s.data, sizeof(format) * s.count);
                break;
            case pipeline_subobject_type::dynamic_pipeline_states:
                s.data = keep(s.data, sizeof(dynamic_state) * s.count);
                break;
            case pipeline_subobject_type::flags:
                s.data = keep(s.data, sizeof(pipeline_flags));
                break;
            case pipeline_subobject_type::sample_mask:
            case pipeline_subobject_type::sample_count:
            case pipeline_subobject_type::viewport_count:
            case pipeline_subobject_type::max_vertex_count:
                s.data = keep(s.data, sizeof(uint32_t));
                break;
            default:
                unknown_type = uint32_t(s.type);
                return false;
            }
            subs.push_back(s);
        }
        return pixel_shader != SIZE_MAX || compute_shader != SIZE_MAX;
    }
};
} // namespace pipeline_copy

using pipeline_copy::stored_desc;
