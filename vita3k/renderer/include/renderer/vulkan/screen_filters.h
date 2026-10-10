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

#pragma once

#include <vkutil/objects.h>

#include <string_view>
#include <vector>

namespace renderer::vulkan {

class ScreenRenderer;
struct Viewport;

class ScreenFilter {
protected:
    ScreenRenderer &screen;

public:
    ScreenFilter(ScreenRenderer &screen_renderer);
    virtual ~ScreenFilter() = default;
    virtual void init() = 0;
    virtual void on_resize() {};
    virtual void render(bool is_pre_renderpass, vk::ImageView src_img, vk::ImageLayout src_layout, const Viewport &viewport) = 0;
    virtual std::string_view get_name() = 0;
    // do we need the render pass not to clear the swapchain content ?
    virtual bool need_post_processing_render_pass() {
        return false;
    }
};

class SinglePassScreenFilter : public ScreenFilter {
private:
    vk::DescriptorSetLayout descriptor_set_layout;
    std::vector<vk::DescriptorSet> descriptor_sets;
    vk::DescriptorPool descriptor_pool;
    vk::Pipeline pipeline;

    vkutil::Buffer vao;
    std::vector<std::array<float, 4>> last_uvs;

protected:
    // write_push_constants() overrides push into this, so it cannot stay private.
    vk::PipelineLayout pipeline_layout;

    vk::ShaderModule vertex_shader;
    vk::ShaderModule fragment_shader;

    vk::Sampler sampler;

    void create_layout_sync();
    void create_graphics_pipeline();

    // Push constants the pipeline is created with. Filters that need more than the default
    // two floats (see the fragment shader's push_constant block) override this, and the
    // pipeline layout is sized from it.
    virtual std::size_t push_constants_size() const;

    // Called once per draw with the same push constant block the pipeline was sized for.
    virtual void write_push_constants(vk::CommandBuffer &cmd, const Viewport &viewport) const;

protected:
    // file name inside shaders-builtin/vulkan
    virtual std::string_view get_vertex_name();
    virtual std::string_view get_fragment_name();
    virtual vk::Sampler create_sampler() = 0;

public:
    SinglePassScreenFilter(ScreenRenderer &screen);
    ~SinglePassScreenFilter();
    void init() override;
    void render(bool is_pre_renderpass, vk::ImageView src_img, vk::ImageLayout src_layout, const Viewport &viewport) override;
};

class NearestScreenFilter : public SinglePassScreenFilter {
protected:
    vk::Sampler create_sampler() override;

public:
    NearestScreenFilter(ScreenRenderer &screen)
        : SinglePassScreenFilter(screen) {}

    std::string_view get_name() override {
        return "Nearest";
    }
};

class BilinearScreenFilter : public SinglePassScreenFilter {
protected:
    vk::Sampler create_sampler() override;

public:
    BilinearScreenFilter(ScreenRenderer &screen)
        : SinglePassScreenFilter(screen) {}

    std::string_view get_name() override {
        return "Bilinear";
    }
};

class BicubicScreenFilter : public SinglePassScreenFilter {
protected:
    // the Bicubic filter uses a custom shader
    std::string_view get_fragment_name() override;
    vk::Sampler create_sampler() override;

public:
    BicubicScreenFilter(ScreenRenderer &screen)
        : SinglePassScreenFilter(screen) {}

    std::string_view get_name() override {
        return "Bicubic";
    }
};

class FXAAScreenFilter : public SinglePassScreenFilter {
protected:
    // the FXAA filter uses a custom shader
    std::string_view get_fragment_name() override;
    vk::Sampler create_sampler() override;

public:
    FXAAScreenFilter(ScreenRenderer &screen)
        : SinglePassScreenFilter(screen) {}

    std::string_view get_name() override {
        return "FXAA";
    }
};

// FSR 1.0 as a fragment shader, for the same reason UE5 has a mobile variant: the compute
// implementation writes the swapchain as a storage image and clears it every frame, which
// is both expensive and unreliable on tiled mobile GPUs. As a screen filter the whole thing
// is one rasterised pass over the destination with no swapchain manipulation at all.
class FSRScreenFilter : public SinglePassScreenFilter {
protected:
    std::string_view get_fragment_name() override;
    vk::Sampler create_sampler() override;
    std::size_t push_constants_size() const override;
    void write_push_constants(vk::CommandBuffer &cmd, const Viewport &viewport) const override;

public:
    FSRScreenFilter(ScreenRenderer &screen)
        : SinglePassScreenFilter(screen) {}

    std::string_view get_name() override {
        return "FSR";
    }
};

} // namespace renderer::vulkan
