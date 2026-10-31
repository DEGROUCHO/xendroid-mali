/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_shader.h"

#include <cstdint>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/ui/vulkan/vulkan_provider.h"

#include "third_party/zstd/lib/zstd.h"

DECLARE_bool(log_vk_memory);

DEFINE_bool(vulkan_release_spirv_after_module, true,
            "Free a shader translation's SPIR-V once its VkShaderModule is "
            "created (tens of MB per title; kept when dump_shaders is set).",
            "Vulkan");

DEFINE_bool(vulkan_shader_module_compressed_spirv, true,
            "When the SPIR-V of a translation is released after its module "
            "is created, keep it zstd-compressed (several times smaller) so "
            "the module itself can be destroyed when idle and rebuilt on "
            "demand.",
            "Vulkan");

namespace xe {
namespace gpu {
namespace vulkan {
namespace {
std::atomic<uint64_t> graphics_shader_modules_created{0};
std::atomic<uint64_t> compressed_spirv_bytes{0};
}

uint64_t VulkanShader::CompressedSpirvBytes() {
  return compressed_spirv_bytes.load(std::memory_order_relaxed);
}

std::atomic<uint64_t> graphics_shader_module_spirv_bytes{0};

uint64_t VulkanShader::CreatedShaderModuleSpirvBytes() {
  return graphics_shader_module_spirv_bytes.load(std::memory_order_relaxed);
}

uint64_t VulkanShader::CreatedShaderModuleCount() {
  return graphics_shader_modules_created.load(std::memory_order_relaxed);
}

VulkanShader::VulkanTranslation::~VulkanTranslation() {
  compressed_spirv_bytes.fetch_sub(compressed_spirv_.size(),
                                   std::memory_order_relaxed);
  if (shader_module_ != VK_NULL_HANDLE) {
    const ui::vulkan::VulkanDevice* const vulkan_device =
        static_cast<const VulkanShader&>(shader()).vulkan_device_;
    vulkan_device->functions().vkDestroyShaderModule(
        vulkan_device->device(), shader_module_,
        ui::vulkan::VulkanDevice::host_allocation_callbacks(
            ui::vulkan::VulkanDevice::HostAllocationCategory::kShaderModule));
  }
}

VkShaderModule VulkanShader::VulkanTranslation::GetOrCreateShaderModule() {
  if (!is_valid()) {
    return VK_NULL_HANDLE;
  }

  // Lock for creation - multiple threads may try to create the same shader
  std::lock_guard<std::mutex> lock(shader_module_mutex_);

  if (shader_module_ != VK_NULL_HANDLE) {
    return shader_module_;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      static_cast<const VulkanShader&>(shader()).vulkan_device_;

  // A module released when idle is rebuilt from the compressed SPIR-V.
  std::vector<uint8_t> decompressed_spirv;
  const std::vector<uint8_t>* spirv = &translated_binary_;
  if (translated_binary_.empty() && !compressed_spirv_.empty()) {
    decompressed_spirv.resize(compressed_spirv_source_size_);
    const size_t decompressed_size = ZSTD_decompress(
        decompressed_spirv.data(), decompressed_spirv.size(),
        compressed_spirv_.data(), compressed_spirv_.size());
    if (ZSTD_isError(decompressed_size) ||
        decompressed_size != compressed_spirv_source_size_) {
      XELOGE(
          "VulkanShader::VulkanTranslation: Failed to decompress the SPIR-V "
          "of shader {:016X} modification {:016X}",
          shader().ucode_data_hash(), modification());
      MakeInvalid();
      return VK_NULL_HANDLE;
    }
    spirv = &decompressed_spirv;
  }

  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = spirv->size();
  shader_module_create_info.pCode =
      reinterpret_cast<const uint32_t*>(spirv->data());

  if (vulkan_device->functions().vkCreateShaderModule(
          vulkan_device->device(), &shader_module_create_info,
          ui::vulkan::VulkanDevice::host_allocation_callbacks(
              ui::vulkan::VulkanDevice::HostAllocationCategory::kShaderModule),
          &shader_module_) != VK_SUCCESS) {
    XELOGE(
        "VulkanShader::VulkanTranslation: Failed to create a Vulkan shader "
        "module for shader {:016X} modification {:016X}",
        shader().ucode_data_hash(), modification());
    MakeInvalid();
    return VK_NULL_HANDLE;
  }

  if (cvars::log_vk_memory) {
    graphics_shader_modules_created.fetch_add(1, std::memory_order_relaxed);
    graphics_shader_module_spirv_bytes.fetch_add(spirv->size(),
                                                 std::memory_order_relaxed);
  }
  // The driver has its own copy now, and nothing reads the SPIR-V again unless
  // it's dumped (right after this).
  if (cvars::vulkan_release_spirv_after_module && cvars::dump_shaders.empty() &&
      !translated_binary_.empty()) {
    if (cvars::vulkan_shader_module_compressed_spirv &&
        compressed_spirv_.empty()) {
      std::vector<uint8_t> compressed(
          ZSTD_compressBound(translated_binary_.size()));
      const size_t compressed_size =
          ZSTD_compress(compressed.data(), compressed.size(),
                        translated_binary_.data(), translated_binary_.size(), 3);
      if (!ZSTD_isError(compressed_size)) {
        compressed.resize(compressed_size);
        compressed.shrink_to_fit();
        compressed_spirv_ = std::move(compressed);
        compressed_spirv_source_size_ = translated_binary_.size();
        compressed_spirv_bytes.fetch_add(compressed_spirv_.size(),
                                         std::memory_order_relaxed);
      }
    }
    std::vector<uint8_t>().swap(translated_binary_);
  }
  return shader_module_;
}

bool VulkanShader::VulkanTranslation::ReleaseShaderModule() {
  std::lock_guard<std::mutex> lock(shader_module_mutex_);
  if (shader_module_ == VK_NULL_HANDLE ||
      (compressed_spirv_.empty() && translated_binary_.empty())) {
    return false;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      static_cast<const VulkanShader&>(shader()).vulkan_device_;
  vulkan_device->functions().vkDestroyShaderModule(
      vulkan_device->device(), shader_module_,
      ui::vulkan::VulkanDevice::host_allocation_callbacks(
          ui::vulkan::VulkanDevice::HostAllocationCategory::kShaderModule));
  shader_module_ = VK_NULL_HANDLE;
  return true;
}

VulkanShader::VulkanShader(const ui::vulkan::VulkanDevice* const vulkan_device,
                           const xenos::ShaderType shader_type,
                           const uint64_t ucode_data_hash,
                           const uint32_t* const ucode_dwords,
                           const size_t ucode_dword_count,
                           const std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian),
      vulkan_device_(vulkan_device) {
  assert_not_null(vulkan_device);
}

Shader::Translation* VulkanShader::CreateTranslationInstance(
    uint64_t modification) {
  return new VulkanTranslation(*this, modification);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
