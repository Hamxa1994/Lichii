// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

#include "video_core/host_shaders/fsr3_flow_median_comp_spv.h"
#include "video_core/host_shaders/fsr3_frame_interpolation_comp_spv.h"
#include "video_core/host_shaders/fsr3_luma_pyramid_comp_spv.h"
#include "video_core/host_shaders/fsr3_optical_flow_comp_spv.h"

namespace Vulkan {

namespace {

// Every image this pipeline owns lives in GENERAL, except briefly during a copy.
constexpr VkAccessFlags ALL_PRIOR_ACCESS = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                           VK_ACCESS_TRANSFER_READ_BIT |
                                           VK_ACCESS_TRANSFER_WRITE_BIT;
constexpr VkPipelineStageFlags ALL_PRIOR_STAGES = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                  VK_PIPELINE_STAGE_TRANSFER_BIT;

// First refinement step of the flow search on each level, in pixels of that level (level 0 is
// the largest). The coarsest level has no parent vector, so it starts widest; with the halvings
// down to FLOW_MIN_STEP its reach is about 7.5 of its own pixels, 60 full-size pixels.
constexpr std::array<float, 3> FLOW_FIRST_STEP{1.0f, 2.0f, 4.0f};
constexpr float FLOW_MIN_STEP = 0.5f;
// Score penalty per pixel of motion that biases ties toward small vectors.
constexpr float FLOW_SMOOTHNESS = 0.002f;

constexpr VkFormat LUMA_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat FLOW_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags src_access,
                                 VkAccessFlags dst_access, VkImageLayout old_layout,
                                 VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
}

/// Moves freshly created (UNDEFINED) images to GENERAL, the layout the rest of the code assumes.
void InitializeLayouts(vk::CommandBuffer cmdbuf, std::span<const VkImage> images) {
    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(images.size());
    for (const VkImage image : images) {
        barriers.push_back(MakeBarrier(image, 0, ALL_PRIOR_ACCESS, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_GENERAL));
    }
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, {}, {}, std::span<const VkImageMemoryBarrier>{barriers});
}

/// Orders one compute dispatch after the previous one: its writes, and the reads it still owes
/// the images it is about to overwrite.
void ComputeBarrier(vk::CommandBuffer cmdbuf) {
    const VkMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
}

/// Makes the flow written by the compute shader visible to the interpolation pass. The image
/// stays GENERAL because the descriptors that sample it declare GENERAL.
void BarrierComputeWriteToRead(vk::CommandBuffer cmdbuf, VkImage image) {
    const auto barrier = MakeBarrier(image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
}

/// Copies one GENERAL image into one or two GENERAL destinations (dest_b may be null), with three
/// barrier calls.
void CopyImageTo(vk::CommandBuffer cmdbuf, VkImage source, VkImage dest_a, VkImage dest_b,
                 VkExtent2D extent,
                 VkPipelineStageFlags final_stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VkAccessFlags final_access = VK_ACCESS_SHADER_READ_BIT) {
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };

    std::array<VkImageMemoryBarrier, 3> before{
        MakeBarrier(source, ALL_PRIOR_ACCESS, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        MakeBarrier(dest_a, ALL_PRIOR_ACCESS, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        MakeBarrier(dest_b, ALL_PRIOR_ACCESS, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    const size_t count = dest_b != VK_NULL_HANDLE ? 3 : 2;
    cmdbuf.PipelineBarrier(ALL_PRIOR_STAGES, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {},
                           std::span<const VkImageMemoryBarrier>{before.data(), count});

    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_a,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
    if (dest_b != VK_NULL_HANDLE) {
        cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_b,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
    }

    std::array<VkImageMemoryBarrier, 3> after{
        MakeBarrier(source, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(dest_a, VK_ACCESS_TRANSFER_WRITE_BIT, final_access,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(dest_b, VK_ACCESS_TRANSFER_WRITE_BIT, final_access,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, final_stages, 0, {}, {},
                           std::span<const VkImageMemoryBarrier>{after.data(), count});
}

[[nodiscard]] u32 GroupCount(u32 size) {
    return (size + 15) / 16;
}

[[nodiscard]] VkExtent2D HalfExtent(VkExtent2D extent) {
    return VkExtent2D{(std::max)(1u, (extent.width + 1) / 2), (std::max)(1u, (extent.height + 1) / 2)};
}

} // anonymous namespace

struct PyramidPushConstants {
    float inv_dest_w;
    float inv_dest_h;
    float dest_w;
    float dest_h;
    float from_color;
    float reserved;
};

struct FlowPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float first_step;
    float min_step;
    float smoothness;
    float has_parent;
};

struct InterpPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float blend_factor;
    float reserved;
};

Fsr3FrameGen::Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                           VkExtent2D extent, VkFormat format)
    : m_allocator{memory_allocator}, m_extent{extent}, m_format{format} {
    // Level 0 is half the frame (the old single-level flow ran there); every further level halves.
    m_level_extents[0] = VkExtent2D{(std::max)(1u, extent.width / 2),
                                    (std::max)(1u, extent.height / 2)};
    for (size_t level = 1; level < LEVELS; ++level) {
        m_level_extents[level] = HalfExtent(m_level_extents[level - 1]);
    }

    CreateResources(device);
    CreateShaders(device);
    CreatePipelines(device);
    WriteDescriptors(device);
    m_initialized = true;
}

Fsr3FrameGen::~Fsr3FrameGen() = default;

void Fsr3FrameGen::CreateResources(const Device& device) {
    m_linear_sampler = CreateBilinearSampler(device);

    // The frame before the current one.
    m_history_image = CreateWrappedImage(m_allocator, m_extent, m_format);
    m_history_view = CreateWrappedImageView(device, m_history_image, m_format);

    // The latest presented frame, kept until the next one arrives. The pyramid is built from this
    // copy, so no descriptor ever has to point at the presented frame's image.
    m_current_image = CreateWrappedImage(m_allocator, m_extent, m_format);
    m_current_view = CreateWrappedImageView(device, m_current_image, m_format);

    m_output_image = CreateWrappedImage(m_allocator, m_extent, m_format);
    m_output_view = CreateWrappedImageView(device, m_output_image, m_format);

    for (size_t pyramid = 0; pyramid < 2; ++pyramid) {
        for (size_t level = 0; level < LEVELS; ++level) {
            m_luma_images[pyramid][level] =
                CreateWrappedImage(m_allocator, m_level_extents[level], LUMA_FORMAT);
            m_luma_views[pyramid][level] =
                CreateWrappedImageView(device, m_luma_images[pyramid][level], LUMA_FORMAT);
        }
    }
    for (size_t level = 0; level < LEVELS; ++level) {
        m_flow_images[level] = CreateWrappedImage(m_allocator, m_level_extents[level], FLOW_FORMAT);
        m_flow_views[level] = CreateWrappedImageView(device, m_flow_images[level], FLOW_FORMAT);
    }
    m_filtered_image = CreateWrappedImage(m_allocator, m_level_extents[0], FLOW_FORMAT);
    m_filtered_view = CreateWrappedImageView(device, m_filtered_image, FLOW_FORMAT);

    // Sets: pyramid (1 sampler + 1 storage) x 6, flow (3 + 1) x 6, median (1 + 1), interpolation (3 + 1).
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 48},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 24},
    };
    m_desc_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 16,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
}

void Fsr3FrameGen::CreateShaders(const Device& device) {
    m_pyramid_shader = BuildShader(device, FSR3_LUMA_PYRAMID_COMP_SPV);
    m_median_shader = BuildShader(device, FSR3_FLOW_MEDIAN_COMP_SPV);
    m_flow_shader = BuildShader(device, FSR3_OPTICAL_FLOW_COMP_SPV);
    m_interp_shader = BuildShader(device, FSR3_FRAME_INTERPOLATION_COMP_SPV);
}

void Fsr3FrameGen::CreatePipelines(const Device& device) {
    const auto make_pipeline = [&](const vk::DescriptorSetLayout& layout, u32 push_size,
                                   const vk::ShaderModule& shader, vk::PipelineLayout& pipe_layout,
                                   vk::Pipeline& pipeline) {
        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = push_size,
        };
        pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = layout.address(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_range,
        });
        pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *shader,
                .pName = "main",
            },
            .layout = *pipe_layout,
        });
    };

    // Luma pyramid: source (sampled), destination (storage).
    const std::array pyramid_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    m_pyramid_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(pyramid_bindings.size()),
        .pBindings = pyramid_bindings.data(),
    });
    make_pipeline(m_pyramid_desc_layout, sizeof(PyramidPushConstants), m_pyramid_shader,
                  m_pyramid_pipe_layout, m_pyramid_pipeline);

    // The median filter reads one flow image and writes another, like a pyramid level.
    make_pipeline(m_pyramid_desc_layout, sizeof(PyramidPushConstants), m_median_shader,
                  m_median_pipe_layout, m_median_pipeline);

    // Optical flow: current luma, previous luma, parent flow (sampled), flow out (storage).
    const std::array flow_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    m_flow_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(flow_bindings.size()),
        .pBindings = flow_bindings.data(),
    });
    make_pipeline(m_flow_desc_layout, sizeof(FlowPushConstants), m_flow_shader, m_flow_pipe_layout,
                  m_flow_pipeline);

    // Frame interpolation: current, previous, flow (sampled), generated frame (storage).
    const std::array interp_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    m_interp_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(interp_bindings.size()),
        .pBindings = interp_bindings.data(),
    });
    make_pipeline(m_interp_desc_layout, sizeof(InterpPushConstants), m_interp_shader,
                  m_interp_pipe_layout, m_interp_pipeline);

    const std::vector<VkDescriptorSetLayout> pyramid_layouts(2 * LEVELS, *m_pyramid_desc_layout);
    const std::vector<VkDescriptorSetLayout> flow_layouts(2 * LEVELS, *m_flow_desc_layout);
    m_pyramid_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, pyramid_layouts);
    m_flow_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, flow_layouts);
    m_median_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, {*m_pyramid_desc_layout});
    m_interp_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, {*m_interp_desc_layout});
}

void Fsr3FrameGen::WriteDescriptors(const Device& device) {
    // Everything but the interpolation's destination is fixed for the life of the object, so the
    // sets are written once and never touched while the GPU may still be using them.
    const auto write = [&](VkDescriptorSet set, std::span<const VkImageView> sampled,
                           VkImageView storage) {
        std::vector<VkDescriptorImageInfo> image_infos;
        image_infos.reserve(sampled.size());
        std::vector<VkWriteDescriptorSet> updates;
        u32 binding = 0;
        for (const VkImageView view : sampled) {
            updates.push_back(
                CreateWriteDescriptorSet(image_infos, *m_linear_sampler, view, set, binding++));
        }
        const VkDescriptorImageInfo storage_info{
            .imageView = storage,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        updates.push_back(VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &storage_info,
        });
        device.GetLogical().UpdateDescriptorSets(updates, {});
    };

    for (size_t pyramid = 0; pyramid < 2; ++pyramid) {
        for (size_t level = 0; level < LEVELS; ++level) {
            const size_t index = pyramid * LEVELS + level;

            const std::array<VkImageView, 1> source{
                level == 0 ? *m_current_view : *m_luma_views[pyramid][level - 1]};
            write(m_pyramid_desc_sets[index], source, *m_luma_views[pyramid][level]);

            // The coarsest level has no parent: it gets some valid view that has_parent = 0 ignores.
            const std::array<VkImageView, 3> flow_inputs{
                *m_luma_views[pyramid][level],
                *m_luma_views[pyramid ^ 1][level],
                level + 1 < LEVELS ? *m_flow_views[level + 1] : *m_flow_views[0],
            };
            write(m_flow_desc_sets[index], flow_inputs, *m_flow_views[level]);
        }
    }

    const std::array<VkImageView, 1> median_input{*m_flow_views[0]};
    write(m_median_desc_sets[0], median_input, *m_filtered_view);

    const std::array<VkImageView, 3> interp_inputs{*m_current_view, *m_history_view,
                                                   *m_filtered_view};
    write(m_interp_desc_sets[0], interp_inputs, *m_output_view);
}

void Fsr3FrameGen::RecordPyramid(vk::CommandBuffer cmdbuf, size_t pyramid) {
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_pyramid_pipeline);
    for (size_t level = 0; level < LEVELS; ++level) {
        // The first level waits for the copy of the new frame and for the flow that last read
        // this pyramid; every other level waits for the one above it.
        ComputeBarrier(cmdbuf);

        const VkExtent2D extent = m_level_extents[level];
        const PyramidPushConstants constants{
            .inv_dest_w = 1.0f / static_cast<float>(extent.width),
            .inv_dest_h = 1.0f / static_cast<float>(extent.height),
            .dest_w = static_cast<float>(extent.width),
            .dest_h = static_cast<float>(extent.height),
            .from_color = level == 0 ? 1.0f : 0.0f,
            .reserved = 0.0f,
        };
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_pyramid_pipe_layout, 0,
                                  m_pyramid_desc_sets[pyramid * LEVELS + level], {});
        cmdbuf.PushConstants(*m_pyramid_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch(GroupCount(extent.width), GroupCount(extent.height), 1);
    }
}

void Fsr3FrameGen::RecordFlow(vk::CommandBuffer cmdbuf, size_t pyramid) {
    // The interpolation of the previous pair may still be reading the filtered flow.
    TransitionImageLayout(cmdbuf, *m_filtered_image, VK_IMAGE_LAYOUT_GENERAL);

    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipeline);
    for (size_t i = 0; i < LEVELS; ++i) {
        const size_t level = LEVELS - 1 - i;
        // Waits for the pyramid on the first pass and for the parent level on the others.
        ComputeBarrier(cmdbuf);

        const VkExtent2D extent = m_level_extents[level];
        const FlowPushConstants constants{
            .inv_render_w = 1.0f / static_cast<float>(extent.width),
            .inv_render_h = 1.0f / static_cast<float>(extent.height),
            .render_w = static_cast<float>(extent.width),
            .render_h = static_cast<float>(extent.height),
            .first_step = FLOW_FIRST_STEP[level],
            .min_step = FLOW_MIN_STEP,
            .smoothness = FLOW_SMOOTHNESS,
            .has_parent = level + 1 < LEVELS ? 1.0f : 0.0f,
        };
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_flow_pipe_layout, 0,
                                  m_flow_desc_sets[pyramid * LEVELS + level], {});
        cmdbuf.PushConstants(*m_flow_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch(GroupCount(extent.width), GroupCount(extent.height), 1);
    }

    // Median filter of the finest level, once per real frame.
    ComputeBarrier(cmdbuf);
    {
        const VkExtent2D extent = m_level_extents[0];
        const PyramidPushConstants constants{
            .inv_dest_w = 1.0f / static_cast<float>(extent.width),
            .inv_dest_h = 1.0f / static_cast<float>(extent.height),
            .dest_w = static_cast<float>(extent.width),
            .dest_h = static_cast<float>(extent.height),
            .from_color = 0.0f,
            .reserved = 0.0f,
        };
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_median_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_median_pipe_layout, 0,
                                  m_median_desc_sets[0], {});
        cmdbuf.PushConstants(*m_median_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch(GroupCount(extent.width), GroupCount(extent.height), 1);
    }

    BarrierComputeWriteToRead(cmdbuf, *m_filtered_image);
}

void Fsr3FrameGen::Process(const Device& device, Scheduler& scheduler, VkImage current_frame,
                           [[maybe_unused]] VkImageView current_view, u64 frame_count) {
    if (!m_has_history) {
        // First frame: put the images in GENERAL, remember this frame and build its pyramid.
        // There is nothing to interpolate toward yet.
        scheduler.Record([this, current_frame](vk::CommandBuffer cmdbuf) {
            std::vector<VkImage> images{*m_history_image, *m_current_image, *m_output_image,
                                        *m_filtered_image};
            for (size_t level = 0; level < LEVELS; ++level) {
                images.push_back(*m_flow_images[level]);
                images.push_back(*m_luma_images[0][level]);
                images.push_back(*m_luma_images[1][level]);
            }
            InitializeLayouts(cmdbuf, images);
            CopyImageTo(cmdbuf, current_frame, *m_history_image, *m_current_image, m_extent);
            RecordPyramid(cmdbuf, 0);
        });
        m_pyramid = 0;
        m_has_history = true;
        m_last_frame_count = frame_count;
        return;
    }

    // Duplicate frame: the flow and both images are still valid.
    if (frame_count == m_last_frame_count && m_flow_valid) {
        return;
    }
    m_last_frame_count = frame_count;

    // The new frame's pyramid goes into the slot of the frame before the previous one, and the
    // flow matches it against the previous frame's pyramid, which is still in the other slot.
    const size_t pyramid = m_pyramid ^ 1;

    scheduler.Record([this, current_frame, pyramid](vk::CommandBuffer cmdbuf) {
        // The frame presented last time becomes the "previous" frame, for the flow and for the
        // frames generated between it and the new one.
        CopyImageTo(cmdbuf, *m_current_image, *m_history_image, VK_NULL_HANDLE, m_extent);
        CopyImageTo(cmdbuf, current_frame, *m_current_image, VK_NULL_HANDLE, m_extent);

        RecordPyramid(cmdbuf, pyramid);
        RecordFlow(cmdbuf, pyramid);
    });

    m_pyramid = pyramid;
    m_flow_valid = true;
}

void Fsr3FrameGen::Generate([[maybe_unused]] const Device& device, Scheduler& scheduler,
                            VkImage destination_image,
                            [[maybe_unused]] VkImageView destination_storage_view,
                            float blend_factor, [[maybe_unused]] float flow_scale) {
    if (!m_has_history || !m_flow_valid) {
        return;
    }

    // The flow is applied in the units of its own image, so the output size alone is needed.
    const u32 out_w = m_extent.width;
    const u32 out_h = m_extent.height;
    const InterpPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(out_w),
        .inv_render_h = 1.0f / static_cast<float>(out_h),
        .render_w = static_cast<float>(out_w),
        .render_h = static_cast<float>(out_h),
        .blend_factor = blend_factor,
        .reserved = 0.0f,
    };

    scheduler.Record([this, constants, out_w, out_h, destination_image](vk::CommandBuffer cmdbuf) {
        // The previous generated frame's copy out of the output image, and the writes of the
        // flow and copies, must be finished before the output image is written again.
        const VkMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = ALL_PRIOR_ACCESS,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(ALL_PRIOR_STAGES, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp_pipe_layout, 0,
                                  m_interp_desc_sets[0], {});
        cmdbuf.PushConstants(*m_interp_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch(GroupCount(out_w), GroupCount(out_h), 1);

        // The presented frame is whatever the output image now holds. Whoever uses the frame
        // next gets it back in GENERAL, visible to any stage.
        CopyImageTo(cmdbuf, *m_output_image, destination_image, VK_NULL_HANDLE, m_extent,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    });
}

} // namespace Vulkan
