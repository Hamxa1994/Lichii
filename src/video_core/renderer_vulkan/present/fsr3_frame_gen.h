// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// Optical-flow frame generation that needs no engine motion vectors or depth.
///
/// Despite the name it is not AMD's FSR 3 frame generation (that needs the game's depth and
/// motion vectors); it is a plain compute pipeline that runs on any GPU:
///  - a three-level luma pyramid of every frame (the previous frame's pyramid is kept, not rebuilt),
///  - a coarse-to-fine block-matching optical flow over that pyramid: the coarsest level finds
///    large motion cheaply, each finer level starts from its parent's vectors and only refines,
///    and every vector is stored with its matching cost,
///  - interpolation between the previous and the latest frame along that flow, falling back to
///    the nearer frame where the flow is unreliable (poor match, occlusion, scene cut),
///  - duplicate-frame detection (no flow for a repeated frame).
class Fsr3FrameGen {
public:
    explicit Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                          VkExtent2D extent, VkFormat format);
    ~Fsr3FrameGen();

    void Process(const Device& device, Scheduler& scheduler, VkImage current_frame,
                 VkImageView current_view, u64 frame_count);

    /// Synthesises the frame at 'blend_factor' between the previous and the latest frame and
    /// copies it into 'destination_image' (a GENERAL image of the presented format and size).
    /// No descriptor is written here: everything the GPU reads is bound once at construction, so
    /// the destination's storage view is not needed.
    void Generate(const Device& device, Scheduler& scheduler, VkImage destination_image,
                  VkImageView destination_storage_view, float blend_factor, float flow_scale);

    [[nodiscard]] bool IsReady() const {
        return m_initialized && m_has_history && m_flow_valid;
    }

private:
    static constexpr size_t LEVELS = 3;

    void CreateResources(const Device& device);
    void CreateShaders(const Device& device);
    void CreatePipelines(const Device& device);
    void WriteDescriptors(const Device& device);

    void RecordPyramid(vk::CommandBuffer cmdbuf, size_t pyramid);
    void RecordFlow(vk::CommandBuffer cmdbuf, size_t pyramid);

    MemoryAllocator& m_allocator;
    VkExtent2D m_extent;
    VkFormat m_format;

    /// Size of every pyramid and flow level; level 0 is half the frame.
    std::array<VkExtent2D, LEVELS> m_level_extents{};

    vk::Sampler m_linear_sampler;
    vk::ShaderModule m_pyramid_shader;
    vk::ShaderModule m_median_shader;
    vk::ShaderModule m_flow_shader;
    vk::ShaderModule m_interp_shader;

    vk::DescriptorSetLayout m_pyramid_desc_layout;
    vk::PipelineLayout m_pyramid_pipe_layout;
    vk::Pipeline m_pyramid_pipeline;

    vk::DescriptorSetLayout m_flow_desc_layout;
    vk::PipelineLayout m_flow_pipe_layout;
    vk::Pipeline m_flow_pipeline;

    // Median filter of the level 0 flow; same bindings as a pyramid level (one sampled, one storage).
    vk::PipelineLayout m_median_pipe_layout;
    vk::Pipeline m_median_pipeline;

    vk::DescriptorSetLayout m_interp_desc_layout;
    vk::PipelineLayout m_interp_pipe_layout;
    vk::Pipeline m_interp_pipeline;

    vk::DescriptorPool m_desc_pool;
    /// One set per (pyramid, level): pyramid[p * LEVELS + level] builds luma level 'level' of
    /// pyramid p; flow[p * LEVELS + level] matches pyramid p (the newest frame) against pyramid
    /// p ^ 1 (the previous one).
    vk::DescriptorSets m_pyramid_desc_sets;
    vk::DescriptorSets m_flow_desc_sets;
    vk::DescriptorSets m_median_desc_sets;
    vk::DescriptorSets m_interp_desc_sets;

    // Previous frame (history) and latest frame (current), full size.
    vk::Image m_history_image;
    vk::ImageView m_history_view;
    vk::Image m_current_image;
    vk::ImageView m_current_view;

    // The interpolation writes here and the result is copied to the presented frame.
    vk::Image m_output_image;
    vk::ImageView m_output_view;

    // Two luma pyramids, used alternately: one holds the newest frame, the other the one before.
    std::array<std::array<vk::Image, LEVELS>, 2> m_luma_images;
    std::array<std::array<vk::ImageView, LEVELS>, 2> m_luma_views;

    // Flow per level: x, y, matching cost of the vector, matching cost of zero motion.
    // Level 0 feeds the median filter.
    std::array<vk::Image, LEVELS> m_flow_images;
    std::array<vk::ImageView, LEVELS> m_flow_views;

    // Level 0 flow after the median filter: what the interpolation reads.
    vk::Image m_filtered_image;
    vk::ImageView m_filtered_view;

    bool m_initialized{};
    bool m_has_history{};

    /// Which pyramid holds the newest frame.
    size_t m_pyramid{};

    // Duplicate-frame detection: skip optical flow when the frame hasn't changed
    u64 m_last_frame_count{};
    bool m_flow_valid{};
};

} // namespace Vulkan
