#include "VulkanInterpolator.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace veyra::linuxgpu {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
}
struct Parameters {
    uint32_t width, height, tilesX, tilesY, radius;
    float phase, cutThreshold;
    uint32_t reserved;
};
static_assert(sizeof(Parameters) == 32);
std::vector<uint32_t> readSpirv(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot read shader: " + path.string());
    const auto size = stream.tellg();
    if (size < 20 || size > 16 * 1024 * 1024 || size % 4 != 0)
        throw std::runtime_error("Invalid SPIR-V size: " + path.string());
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(words.data()), size) || words[0] != 0x07230203)
        throw std::runtime_error("Invalid SPIR-V: " + path.string());
    return words;
}
}

struct VulkanInterpolator::Impl {
    struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{}; VkDeviceSize size{}; };
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool commandPool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkDescriptorSetLayout descriptorLayout{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSet descriptors{};
    VkPipelineLayout pipelineLayout{};
    std::array<VkPipeline, 2> pipelines{};
    std::array<Buffer, 5> buffers{};
    std::string name;
    Parameters parameters{};
    size_t pixels{};
    bool poisoned = false;

    ~Impl() {
        if (device) {
            vkDeviceWaitIdle(device);
            for (auto pipeline : pipelines) if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            for (auto& buffer : buffers) {
                if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
                if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
                if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void allocate(Buffer& out, VkDeviceSize size) {
        out.size = size;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        check(vkCreateBuffer(device, &info, nullptr, &out.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        uint32_t type = std::numeric_limits<uint32_t>::max();
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const auto flags = memoryProperties.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = i; break;
            }
        }
        if (type == std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Diagnostic harness needs host-coherent storage memory");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &allocation, nullptr, &out.memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device, out.buffer, out.memory, 0), "vkBindBufferMemory");
        check(vkMapMemory(device, out.memory, 0, size, 0, &out.mapped), "vkMapMemory");
    }

    void initialize(uint32_t width, uint32_t height, const std::filesystem::path& shaders) {
        if (!width || !height || width > 4096 || height > 4096)
            throw std::invalid_argument("Reference backend accepts dimensions 1..4096");
        pixels = size_t(width) * height;
        parameters = {width, height, (width + 7) / 8, (height + 7) / 8, 8, .5f, .20f, 0};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Veyra Linux interpolation proof";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        uint32_t family = 0;
        int bestScore = -1;
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_1 ||
                properties.limits.maxComputeWorkGroupInvocations < 256 ||
                properties.limits.maxComputeWorkGroupSize[0] < 16 ||
                properties.limits.maxComputeWorkGroupSize[1] < 16 ||
                properties.limits.maxStorageBufferRange < pixels * 4) continue;
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (uint32_t i = 0; i < familyCount; ++i) {
                int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
                if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && families[i].queueCount && score > bestScore) {
                    physical = candidate; family = i; bestScore = score; name = properties.deviceName;
                }
            }
        }
        if (!physical) throw std::runtime_error("No suitable Vulkan 1.1 compute device found");
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
        const size_t flowBytes = size_t(parameters.tilesX) * parameters.tilesY * 2 * 16;
        const std::array<VkDeviceSize, 5> sizes{pixels * 4, pixels * 4, flowBytes, pixels * 4, 8};
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (uint32_t i = 0; i < 5; ++i) {
            allocate(buffers[i], sizes[i]);
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 5; layoutInfo.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorLayout), "vkCreateDescriptorSetLayout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = descriptorPool; setInfo.descriptorSetCount = 1; setInfo.pSetLayouts = &descriptorLayout;
        check(vkAllocateDescriptorSets(device, &setInfo, &descriptors), "vkAllocateDescriptorSets");
        std::array<VkDescriptorBufferInfo, 5> bufferInfos{};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t i = 0; i < 5; ++i) {
            bufferInfos[i] = {buffers[i].buffer, 0, buffers[i].size};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = descriptors; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &bufferInfos[i];
        }
        vkUpdateDescriptorSets(device, 5, writes.data(), 0, nullptr);
        VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1; pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1; pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
        const std::array<const char*, 2> stages{"motion.spv", "interpolate.spv"};
        for (size_t i = 0; i < stages.size(); ++i) {
            auto words = readSpirv(shaders / stages[i]);
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = words.size() * 4; moduleInfo.pCode = words.data();
            VkShaderModule module{};
            check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.layout = pipelineLayout;
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = module; pipelineInfo.stage.pName = "main";
            auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipelines[i]);
            vkDestroyShaderModule(device, module, nullptr);
            check(result, "vkCreateComputePipelines");
        }
        VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        commandPoolInfo.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &commandInfo, &command), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
    }

    std::vector<uint32_t> run(std::span<const uint32_t> a, std::span<const uint32_t> b, float phase, uint32_t radius) {
        if (poisoned) throw std::runtime_error("GPU submission failed; recreate the interpolator");
        if (a.size() != pixels || b.size() != pixels || !std::isfinite(phase) || phase < 0 || phase > 1 || radius > 16)
            throw std::invalid_argument("Invalid frame extent, phase (0..1), or search radius (0..16)");
        std::memcpy(buffers[0].mapped, a.data(), pixels * 4);
        std::memcpy(buffers[1].mapped, b.data(), pixels * 4);
        std::memset(buffers[4].mapped, 0, 8);
        parameters.phase = phase; parameters.radius = radius;
        check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptors, 0, nullptr);
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[0]);
        vkCmdDispatch(command, (parameters.tilesX + 7) / 8, (parameters.tilesY + 7) / 8, 2);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[1]);
        vkCmdDispatch(command, (parameters.width + 15) / 16, (parameters.height + 15) / 16, 1);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        check(vkResetFences(device, 1, &fence), "vkResetFences");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        poisoned = true;
        check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, 30'000'000'000ULL), "vkWaitForFences (30s)");
        poisoned = false;
        std::vector<uint32_t> output(pixels);
        std::memcpy(output.data(), buffers[3].mapped, pixels * 4);
        return output;
    }
};

VulkanInterpolator::VulkanInterpolator(uint32_t width, uint32_t height, const std::filesystem::path& shaders)
    : impl_(std::make_unique<Impl>()) { impl_->initialize(width, height, shaders); }
VulkanInterpolator::~VulkanInterpolator() = default;
std::vector<uint32_t> VulkanInterpolator::interpolate(std::span<const uint32_t> a, std::span<const uint32_t> b,
                                                     float phase, uint32_t radius) {
    return impl_->run(a, b, phase, radius);
}
const std::string& VulkanInterpolator::deviceName() const { return impl_->name; }
}
