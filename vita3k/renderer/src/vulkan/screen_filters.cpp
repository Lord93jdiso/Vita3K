// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "renderer/vulkan/screen_filters.h"
#include "renderer/vulkan/screen_renderer.h"

#include "renderer/vulkan/state.h"

#include <algorithm>
#include <cmath>

namespace renderer::vulkan {

ScreenFilter::ScreenFilter(ScreenRenderer &screen_renderer)
    : screen(screen_renderer) {}

struct screen_vertex {
    float pos[3];
    float uv[2];
};

static constexpr size_t screen_vertex_size = sizeof(screen_vertex);
static constexpr uint32_t screen_vertex_count = 4;

using screen_vertices_t = screen_vertex[screen_vertex_count];

SinglePassScreenFilter::SinglePassScreenFilter(ScreenRenderer &screen)
    : ScreenFilter(screen) {}

SinglePassScreenFilter::~SinglePassScreenFilter() {
    vk::Device device = screen.state.device;
    // this will only happen when the user changes the option in the GUI, we can afford to waitIdle
    device.waitIdle();
    device.destroy(pipeline);
    device.destroy(pipeline_layout);
    device.destroy(descriptor_pool);
    device.destroy(descriptor_set_layout);
    device.destroy(fragment_shader);
    device.destroy(vertex_shader);

    device.destroy(sampler);
}

// Default push constant block: the 1/size of the source, which is all the built-in filters
// need. Filters with more constants override push_constants_size and write_push_constants.
std::size_t SinglePassScreenFilter::push_constants_size() const {
    return 2 * sizeof(float);
}

void SinglePassScreenFilter::write_push_constants(vk::CommandBuffer &cmd, const Viewport &viewport) const {
    const std::array<float, 2> inv_size = { 1.f / viewport.texture_width, 1.f / viewport.texture_height };
    cmd.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, inv_size.size() * sizeof(float), inv_size.data());
}

void SinglePassScreenFilter::create_layout_sync() {
    vk::Device device = screen.state.device;

    vk::DescriptorSetLayoutBinding sampler_layout_binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    vk::DescriptorSetLayoutCreateInfo descriptor_info{};
    descriptor_info.setBindings(sampler_layout_binding);
    descriptor_set_layout = device.createDescriptorSetLayout(descriptor_info);

    vk::DescriptorPoolSize pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = screen.swapchain_size
    };
    vk::DescriptorPoolCreateInfo pool_info{
        .maxSets = screen.swapchain_size,
    };
    pool_info.setPoolSizes(pool_size);
    descriptor_pool = device.createDescriptorPool(pool_info);

    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = descriptor_pool,
    };
    std::vector<vk::DescriptorSetLayout> descr_set_layouts(screen.swapchain_size, descriptor_set_layout);
    descr_set_info.setSetLayouts(descr_set_layouts);
    descriptor_sets = device.allocateDescriptorSets(descr_set_info);

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(descriptor_set_layout);
    // add push constant for fxaa pipeline, not used by the normal pipeline
    vk::PushConstantRange push_constant{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = static_cast<uint32_t>(push_constants_size()),
    };
    layout_info.setPushConstantRanges(push_constant);
    pipeline_layout = device.createPipelineLayout(layout_info);

    // create vao
    vao.size = sizeof(screen_vertices_t) * screen.swapchain_size;
    vao.init_buffer(vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst);

    // create and zero-fill uvs
    last_uvs.resize(screen.swapchain_size);
    std::fill(last_uvs.begin(), last_uvs.end(), std::array<float, 4>());
}

void SinglePassScreenFilter::create_graphics_pipeline() {
    // create shader modules

    fs::path builtin_shaders_path = screen.state.static_assets / "shaders-builtin/vulkan";
    const auto vertex_shader_path = builtin_shaders_path / get_vertex_name();
    const auto fragment_shader_path = builtin_shaders_path / get_fragment_name();

    vertex_shader = vkutil::load_shader(screen.state.device, vertex_shader_path);
    fragment_shader = vkutil::load_shader(screen.state.device, fragment_shader_path);
    vk::PipelineShaderStageCreateInfo vert_info{
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = vertex_shader,
        .pName = "main"
    };
    vk::PipelineShaderStageCreateInfo frag_info{
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = fragment_shader,
        .pName = "main"
    };
    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = { vert_info, frag_info };

    vk::VertexInputBindingDescription binding_descr{
        .binding = 0,
        .stride = screen_vertex_size,
        .inputRate = vk::VertexInputRate::eVertex
    };
    std::array<vk::VertexInputAttributeDescription, 2> attr_descr;
    // pos
    attr_descr[0] = vk::VertexInputAttributeDescription{
        .location = 0,
        .binding = 0,
        .format = vk::Format::eR32G32B32Sfloat,
        .offset = offsetof(screen_vertex, pos)
    };
    // uv
    attr_descr[1] = vk::VertexInputAttributeDescription{
        .location = 1,
        .binding = 0,
        .format = vk::Format::eR32G32Sfloat,
        .offset = offsetof(screen_vertex, uv)
    };
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding_descr);
    vertex_input.setVertexAttributeDescriptions(attr_descr);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleStrip
    };
    vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1,
        .scissorCount = 1
    };
    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f
    };
    vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1
    };
    vk::PipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask = vkutil::default_color_mask
    };
    vk::PipelineColorBlendStateCreateInfo color_blending{};
    color_blending.setAttachments(blend_attachment);

    // dynamic scissor and viewport are used, in case the extent changes
    static vk::DynamicState dynamic_states[] = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor
    };
    vk::PipelineDynamicStateCreateInfo dynamic_info{};
    dynamic_info.setDynamicStates(dynamic_states);

    vk::GraphicsPipelineCreateInfo pipeline_info{
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = pipeline_layout,
        .renderPass = screen.default_render_pass,
        .subpass = 0
    };
    pipeline_info.setStages(shader_stages);

    const auto result = screen.state.device.createGraphicsPipeline(VK_NULL_HANDLE, pipeline_info);
    if (result.result != vk::Result::eSuccess) {
        LOG_CRITICAL("Failed to create pipeline.");
    }
    pipeline = result.value;
}

std::string_view SinglePassScreenFilter::get_vertex_name() {
    return "render_main.vert.spv";
}

std::string_view SinglePassScreenFilter::get_fragment_name() {
    return "render_main.frag.spv";
}

void SinglePassScreenFilter::init() {
    create_layout_sync();
    create_graphics_pipeline();
    this->sampler = create_sampler();
}

void SinglePassScreenFilter::render(bool is_pre_renderpass, vk::ImageView src_img, vk::ImageLayout src_layout, const Viewport &viewport) {
    if (is_pre_renderpass) {
        std::array<float, 4> uvs = {
            viewport.offset_x / (float)viewport.texture_width,
            viewport.offset_y / (float)viewport.texture_height,
            (viewport.offset_x + viewport.width) / (float)(viewport.texture_width),
            (viewport.offset_y + viewport.height) / (float)(viewport.texture_height)
        };

        // if necessary update vao (should not happen often)
        if (uvs != last_uvs[screen.swapchain_image_idx]) {
            screen_vertices_t vertex_buffer_data = {
                { { 1.f, -1.f, 0.0f }, { 1.f, 0.f } },
                { { -1.f, -1.f, 0.0f }, { 0.f, 0.f } },
                { { 1.f, 1.f, 0.0f }, { 1.f, 1.f } },
                { { -1.f, 1.f, 0.0f }, { 0.f, 1.f } },
            };
            vertex_buffer_data[0].uv[0] = uvs[2];
            vertex_buffer_data[0].uv[1] = uvs[1];

            vertex_buffer_data[1].uv[0] = uvs[0];
            vertex_buffer_data[1].uv[1] = uvs[1];

            vertex_buffer_data[2].uv[0] = uvs[2];
            vertex_buffer_data[2].uv[1] = uvs[3];

            vertex_buffer_data[3].uv[0] = uvs[0];
            vertex_buffer_data[3].uv[1] = uvs[3];

            screen.current_cmd_buffer.updateBuffer(vao.buffer, screen.swapchain_image_idx * sizeof(screen_vertices_t), sizeof(screen_vertices_t), &vertex_buffer_data);
            last_uvs[screen.swapchain_image_idx] = uvs;
        }

        return;
    }

    {
        // update descriptor set
        vk::DescriptorImageInfo descr_image_info{
            .sampler = sampler,
            .imageView = src_img,
            .imageLayout = src_layout,
        };
        vk::WriteDescriptorSet write_descr{
            .dstSet = descriptor_sets[screen.swapchain_image_idx],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        };
        write_descr.setImageInfo(descr_image_info);
        screen.state.device.updateDescriptorSets(write_descr, {});

        // set viewport and scissor
        vk::Rect2D vk_scissor{
            .offset = { 0, 0 },
            .extent = screen.extent
        };
        screen.current_cmd_buffer.setScissor(0, vk_scissor);
        vk::Viewport vk_viewport{
            .minDepth = 0.0f,
            .maxDepth = 1.0f
        };
        // compute viewport now
        const float window_aspect = static_cast<float>(screen.extent.width) / screen.extent.height;
        constexpr float vita_aspect = static_cast<float>(DEFAULT_RES_WIDTH) / DEFAULT_RES_HEIGHT;
        const bool fullscreen_hd_res_pixel_perfect_en = screen.state.fullscreen_hd_res_pixel_perfect && screen.state.fullscreen && !(screen.extent.width % DEFAULT_RES_WIDTH) && !(screen.extent.height % (DEFAULT_RES_HEIGHT - 4));
        if (screen.state.stretch_the_display_area && !fullscreen_hd_res_pixel_perfect_en) {
            // Match the aspect ratio to the screen size.
            vk_viewport.width = static_cast<float>(screen.extent.width);
            vk_viewport.height = static_cast<float>(screen.extent.height);
            vk_viewport.x = 0.0f;
            vk_viewport.y = 0.0f;
        } else if ((window_aspect > vita_aspect) && !fullscreen_hd_res_pixel_perfect_en) {
            // Window is wide. Pin top and bottom.
            vk_viewport.width = screen.extent.height * vita_aspect;
            vk_viewport.height = static_cast<float>(screen.extent.height);
            vk_viewport.x = (screen.extent.width - vk_viewport.width) / 2.0f;
            vk_viewport.y = 0.0f;
        } else {
            // Window is tall. Pin left and right.
            vk_viewport.width = static_cast<float>(screen.extent.width);
            vk_viewport.height = screen.extent.width / vita_aspect;
            vk_viewport.x = 0.0f;
            vk_viewport.y = (screen.extent.height - vk_viewport.height) / 2;
        }
        screen.current_cmd_buffer.setViewport(0, vk_viewport);
    }

    {
        vk::DeviceSize offset = screen.swapchain_image_idx * sizeof(screen_vertices_t);
        screen.current_cmd_buffer.bindVertexBuffers(0, vao.buffer, offset);

        screen.current_cmd_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        screen.current_cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, descriptor_sets[screen.swapchain_image_idx], {});

        write_push_constants(screen.current_cmd_buffer, viewport);

        screen.current_cmd_buffer.draw(4, 1, 0, 0);
    }
}

std::string_view FSRScreenFilter::get_fragment_name() {
    return "fsr_fragment_upscale.frag.spv";
}

vk::Sampler FSRScreenFilter::create_sampler() {
    // Nearest: every sample in the shader addresses an exact texel, so filtering between them
    // would only blur the input before EASU gets to reconstruct it.
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

namespace {

// Mirrors the fragment shader's push_constant block.
struct FsrPushConstants {
    // con0.xy = output-to-input pixel scale, con0.zw = half-pixel offset
    float con0[4];
    // con1.xy = 1 / source size in pixels
    float con1[4];
    // Padding for the shader's vec2 slot, which it no longer reads.
    float pad[2];
    // RCAS sharpness in linear form, where 0 is the sharpest
    float sharp;
};

} // namespace

std::size_t FSRScreenFilter::push_constants_size() const {
    return sizeof(FsrPushConstants);
}

void FSRScreenFilter::write_push_constants(vk::CommandBuffer &cmd, const Viewport &viewport) const {
    const float input_width = static_cast<float>(viewport.texture_width);
    const float input_height = static_cast<float>(viewport.texture_height);
    const float output_width = static_cast<float>(screen.extent.width);
    const float output_height = static_cast<float>(screen.extent.height);

    FsrPushConstants pc{};
    // Straight from FsrEasuCon().
    pc.con0[0] = input_width / output_width;
    pc.con0[1] = input_height / output_height;
    pc.con0[2] = 0.5f * input_width / output_width - 0.5f;
    pc.con0[3] = 0.5f * input_height / output_height - 0.5f;
    pc.con1[0] = 1.f / input_width;
    pc.con1[1] = 1.f / input_height;
    pc.con1[2] = 0.f;
    pc.con1[3] = 0.f;
    pc.pad[0] = 0.f;
    pc.pad[1] = 0.f;

    // FsrRcasCon() turns the stop count into a linear factor by halving once per stop.
    const float stops = std::max(screen.state.fsr_sharpness, 0.0f);
    pc.sharp = std::exp2(-stops);

    cmd.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(pc), &pc);
}

vk::Sampler NearestScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

vk::Sampler BilinearScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

std::string_view BicubicScreenFilter::get_fragment_name() {
    return "render_main_bicubic.frag.spv";
}

vk::Sampler BicubicScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

std::string_view FXAAScreenFilter::get_fragment_name() {
    return "render_main_fxaa.frag.spv";
}

vk::Sampler FXAAScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}


} // namespace renderer::vulkan
