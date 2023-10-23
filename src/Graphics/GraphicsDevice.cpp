#include "GraphicsDevice.hpp"
#include "ReadFile.hpp"
#include "Renderer.hpp"
#include "Model.hpp"
#include "Texture2D.hpp"

#include "stb_image.h"
#include "tiny_gltf.h"

#include <vulkan/vulkan.h>

#include <iostream>
#include <set>

#ifdef _DEBUG
#define LOG_ERROR(x, message) if(!x) { std::cout<<message<<std::endl; exit(1);}
#define LOG_WARN(x, message) if(!x) { std::cout<<message<<std::endl;}
#define LOG_INFO(message) { std::cout<<message<<std::endl;}
#else
#define LOG_ERROR(x, message)
#define LOG_WARN(x, message)
#define LOG_INFO(message)
#endif


namespace Diffuse {
    void FramebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto graphics = reinterpret_cast<GraphicsDevice*>(glfwGetWindowUserPointer(window));
        graphics->SetFramebufferResized(true);
    }

    GraphicsDevice::GraphicsDevice(Config config) {
        // === Initializing GLFW ===
        {
            int result = glfwInit();
            LOG_ERROR(result == GLFW_TRUE, "Failed to intitialize GLFW");
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            m_window = std::make_unique<Window>();
            glfwSetWindowUserPointer(m_window->window(), this);
            glfwSetFramebufferSizeCallback(m_window->window(), FramebufferResizeCallback);
        }
        
        // Check for validation layer support
        if (config.enable_validation_layers && !vkUtilities::CheckValidationLayerSupport(config.validation_layers)) {
        	std::cout << "validation layers requested, but not available!";
        	assert(false);
        }

        // === Create Vulkan Instancee ===
        {
            VkApplicationInfo app_info{};
            app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app_info.pApplicationName = "Diffuse Vulkan Renderer";
            app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
            app_info.pEngineName = "Diffuse";
            app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
            app_info.apiVersion = VK_API_VERSION_1_0;

            std::vector<const char*> extensions = vkUtilities::GetRequiredExtensions(config.enable_validation_layers); // TODO: add a boolean for if validation layers is enabled

            VkInstanceCreateInfo instance_create_info{};
            instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            instance_create_info.pApplicationInfo = &app_info;
            instance_create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            instance_create_info.ppEnabledExtensionNames = extensions.data();
            // TODO: Use multiple validation layers as a backup 
            if (config.enable_validation_layers) {
                instance_create_info.enabledLayerCount = static_cast<uint32_t>(config.validation_layers.size());
                instance_create_info.ppEnabledLayerNames = config.validation_layers.data();
            }

            if (config.enable_validation_layers) {
                m_debug_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
                m_debug_create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                m_debug_create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                vkUtilities::PopulateDebugMessengerCreateInfo(m_debug_create_info);

                instance_create_info.pNext = &m_debug_create_info;
            }

            if (vkCreateInstance(&instance_create_info, nullptr, &m_instance) != VK_SUCCESS) {
                LOG_ERROR(false, "Failed to create Vulkan instance!");
            }
        }

        // === Setup Debug Messenger ===
        {
            if (config.enable_validation_layers) {
                if (vkUtilities::CreateDebugUtilsMessengerEXT(m_instance, &m_debug_create_info, nullptr, &m_debug_messenger) != VK_SUCCESS) {
                    LOG_WARN(false, "**Failed to set up debug messenger**");
                }
            }
        }
        // === Create Surface ===
        if (glfwCreateWindowSurface(m_instance, m_window->window(), nullptr, &m_surface) != VK_SUCCESS) {
            LOG_ERROR(false, "Failed to create window surface!");
        }

        // === Pick Physical Device ===
        {
            uint32_t device_count = 0;
            vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr);
            if (device_count == 0) {
                LOG_ERROR(false, "failed to find GPUs with Vulkan support!");
            }
            std::vector<VkPhysicalDevice> devices(device_count);
            vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data());
            for (const auto& device : devices) {
                if (vkUtilities::IsDeviceSuitable(device, m_surface, config.required_device_extensions)) {
                    m_physical_device = device;
                    break;
                }
            }
            if (m_physical_device == VK_NULL_HANDLE) {
                LOG_ERROR(false, "Failed to find a suitable GPU!");
            }
        }

        // === Create Logical Device ===
        {
            QueueFamilyIndices indices = vkUtilities::FindQueueFamilies(m_physical_device, m_surface);
            std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
            std::set<uint32_t> unique_queue_families = { indices.graphicsFamily.value(), indices.presentFamily.value() };
            float queue_priority = 1.0f;
            for (uint32_t queue_family : unique_queue_families) {
                VkDeviceQueueCreateInfo queue_create_info{};
                queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                queue_create_info.queueFamilyIndex = queue_family;
                queue_create_info.queueCount = 1;
                queue_create_info.pQueuePriorities = &queue_priority;
                queue_create_infos.push_back(queue_create_info);
            }

            VkPhysicalDeviceFeatures device_features{};
            device_features.samplerAnisotropy = VK_TRUE;
            VkDeviceCreateInfo device_create_info{};
            device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            device_create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
            device_create_info.pQueueCreateInfos = queue_create_infos.data();
            device_create_info.pEnabledFeatures = &device_features;
            device_create_info.enabledExtensionCount = static_cast<uint32_t>(config.required_device_extensions.size());
            device_create_info.ppEnabledExtensionNames = config.required_device_extensions.data();
            if (config.enable_validation_layers) {
                device_create_info.enabledLayerCount = static_cast<uint32_t>(config.validation_layers.size());
                device_create_info.ppEnabledLayerNames = config.validation_layers.data();
            }
            else {
                device_create_info.enabledLayerCount = 0;
            }
            if (vkCreateDevice(m_physical_device, &device_create_info, nullptr, &m_device) != VK_SUCCESS) {
                LOG_ERROR(false, "Failed to create logical device!");
            }
            vkGetDeviceQueue(m_device, indices.graphicsFamily.value(), 0, &m_graphics_queue);
            vkGetDeviceQueue(m_device, indices.presentFamily.value(), 0, &m_present_queue);
        }

        // Create Command Pool
        {
            QueueFamilyIndices queueFamilyIndices = vkUtilities::FindQueueFamilies(m_physical_device, m_surface);
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

            if (vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool) != VK_SUCCESS) {
                LOG_ERROR(false, "Failed to create command pool!");
            }
        }

        // === Create Sync Obects ===
        {
            m_render_complete_semaphores.resize(m_render_ahead);
            m_present_complete_semaphores.resize(m_render_ahead);
            m_wait_fences.resize(m_render_ahead);

            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            for (size_t i = 0; i < m_render_ahead; i++) {
                if (vkCreateSemaphore(m_device, &semaphore_info, nullptr, &m_render_complete_semaphores[i]) != VK_SUCCESS ||
                    vkCreateSemaphore(m_device, &semaphore_info, nullptr, &m_present_complete_semaphores[i]) != VK_SUCCESS ||
                    vkCreateFence(m_device, &fence_info, nullptr, &m_wait_fences[i]) != VK_SUCCESS) {
                    LOG_ERROR(false, "Failed to create synchronization objects for a frame!");
                }
            }
        }
        // SUCCESS
    }

    void GraphicsDevice::Setup() {
        TextureSampler sampler{};
        sampler.mag_filter = VK_FILTER_LINEAR;
        sampler.min_filter = VK_FILTER_LINEAR;
        sampler.address_modeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.address_modeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.address_modeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Texture2D* hdr = new Texture2D("../assets/environment.hdr", VK_FORMAT_R32G32B32A32_SFLOAT, sampler, 0, this);
        //white_texture = new Texture2D("../assets/white.jpeg", VK_FORMAT_R8G8B8A8_UNORM, this);
        // === Create Swap Chain ===
        m_swapchain = std::make_unique<Swapchain>(this);
        m_swapchain->Initialize();

        // === Create Render Pass ===
        {
            VkAttachmentDescription color_attachment{};
            color_attachment.format = m_swapchain->GetFormat();
            color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            VkAttachmentDescription depth_attachment{};
            depth_attachment.format = vkUtilities::FindDepthFormat(m_physical_device);
            depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkAttachmentReference color_attachment_ref{};
            color_attachment_ref.attachment = 0;
            color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkAttachmentReference depthAttachmentRef{};
            depthAttachmentRef.attachment = 1;
            depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color_attachment_ref;
            subpass.pDepthStencilAttachment = &depthAttachmentRef;

            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

            std::array<VkAttachmentDescription, 2> attachments = { color_attachment, depth_attachment };
            VkRenderPassCreateInfo render_pass_info{};
            render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            render_pass_info.attachmentCount = static_cast<uint32_t>(attachments.size());
            render_pass_info.pAttachments = attachments.data();
            render_pass_info.subpassCount = 1;
            render_pass_info.pSubpasses = &subpass;
            render_pass_info.dependencyCount = 1;
            render_pass_info.pDependencies = &dependency;

            if (vkCreateRenderPass(m_device, &render_pass_info, nullptr, &m_render_pass) != VK_SUCCESS) {
                LOG_ERROR(false, "Failed to create render pass!");
            }
        }

        // === Create Depth Resource ===
        VkFormat depthFormat = vkUtilities::FindDepthFormat(m_physical_device);
        vkUtilities::CreateImage(m_swapchain->GetExtentWidth(), m_swapchain->GetExtentHeight(), m_device, m_physical_device, depthFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_depth_image, m_depth_image_memory, 1, 1);
        m_depth_image_view = vkUtilities::CreateImageView(m_depth_image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, m_device, 1, 0, 1);

        // === Create Framebuffers ===
        {
            m_framebuffers.resize(m_swapchain->GetSwapchainImageViews().size());

            for (size_t i = 0; i < m_swapchain->GetSwapchainImageViews().size(); i++) {
                std::array<VkImageView, 2> attachments = {
                    m_swapchain->GetSwapchainImageView(i),
                    m_depth_image_view
                };

                VkFramebufferCreateInfo framebuffer_info{};
                framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebuffer_info.renderPass = m_render_pass;
                framebuffer_info.attachmentCount = static_cast<uint32_t>(attachments.size());
                framebuffer_info.pAttachments = attachments.data();
                framebuffer_info.width = m_swapchain->GetExtentWidth();
                framebuffer_info.height = m_swapchain->GetExtentHeight();
                framebuffer_info.layers = 1;

                if (vkCreateFramebuffer(m_device, &framebuffer_info, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
                    LOG_ERROR(false, "Failed to create framebuffer!");
                }
            }
        }

        // === Create Command Buffers ===
        {
            m_command_buffers.resize(m_swapchain->GetSwapchainImages().size());
            VkCommandBufferAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            alloc_info.commandPool = m_command_pool;
            alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc_info.commandBufferCount = (uint32_t)m_command_buffers.size();

            if (vkAllocateCommandBuffers(m_device, &alloc_info, m_command_buffers.data()) != VK_SUCCESS) {
                LOG_ERROR(false, "Failed to allocate command buffers!");
            }
        }

        CreateUniformBuffer();

        uint32_t imageSamplerCount = 0;
        uint32_t materialCount = 0;
        uint32_t meshCount = 0;

        for (auto& material : m_models[0]->GetMaterials()) {
            imageSamplerCount += 5;
            materialCount++;
        }
        for (auto node : m_models[0]->GetLinearNodes()) {
            if (node->mesh) {
                meshCount++;
            }
        }

        const std::array<VkDescriptorPoolSize, 2> poolSizes = { {
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageSamplerCount * m_swapchain->GetImageCount() + 2 },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (4 + meshCount) * m_swapchain->GetImageCount() },
                //{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 16 },
            } };

        VkDescriptorPoolCreateInfo createInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        createInfo.maxSets = (2 + meshCount + materialCount) * m_swapchain->GetImageCount();
        createInfo.poolSizeCount = (uint32_t)poolSizes.size();
        createInfo.pPoolSizes = poolSizes.data();
        if (vkCreateDescriptorPool(m_device, &createInfo, nullptr, &m_descriptor_pools.scene)) {
            throw std::runtime_error("Failed to create descriptor pool");
        }

        VkPipelineCacheCreateInfo pipelineCacheCreateInfo = {};
        pipelineCacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        if (vkCreatePipelineCache(m_device, &pipelineCacheCreateInfo, nullptr, &m_pipeline_cache)) {
            throw std::runtime_error("Failed to create descriptor pool");
        }

        std::vector<VkDescriptorSetLayoutBinding> set_layout_bindings = {
                { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_VERTEX_BIT,   nullptr },
                { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        };

        //VkDescriptorSetLayoutBinding ubo_layout_binding{};
        //ubo_layout_binding.binding = 0;
        //ubo_layout_binding.descriptorCount = 1;
        //ubo_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        //ubo_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        //ubo_layout_binding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI{};
        descriptorSetLayoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorSetLayoutCI.pBindings = set_layout_bindings.data();
        descriptorSetLayoutCI.bindingCount = set_layout_bindings.size();
        if (vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutCI, nullptr, &m_descriptorSetLayouts.scene)) {
            throw std::runtime_error("Failed to create descriptor pool");
        }

        // Descriptor set
        //std::vector<VkDescriptorSetLayout> layouts(m_render_ahead, m_descriptorSetLayouts.scene);
        //m_descriptor_sets.scene.resize(m_render_ahead);

        for (size_t i = 0; i < m_models[0]->GetMaterials().size(); i++) {
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = m_descriptor_pools.scene;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &m_descriptorSetLayouts.scene;

            if (vkAllocateDescriptorSets(m_device, &allocInfo, &(m_models[0]->GetMaterial(i).descriptorSet)) != VK_SUCCESS) {
                throw std::runtime_error("failed to allocate descriptor sets!");
            }

            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = m_ubo.uniformBuffers[0];
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UBO);

            //VkDescriptorImageInfo temp = m_models[0]->GetMaterial(0).baseColorTexture->m_descriptor;
            std::vector<VkDescriptorImageInfo> image_descriptors = {
                m_models[0]->GetMaterial(i).baseColorTexture->m_descriptor,
                m_models[0]->GetMaterial(i).metallicRoughnessTexture->m_descriptor,
                m_models[0]->GetMaterial(i).normalTexture->m_descriptor,
                m_models[0]->GetMaterial(i).occlusionTexture->m_descriptor,
                m_models[0]->GetMaterial(i).emissiveTexture->m_descriptor,
            };

            std::vector<VkWriteDescriptorSet> descriptorWrites;
            descriptorWrites.resize(6);
            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrites[0].dstSet = m_models[0]->GetMaterial(i).descriptorSet;
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pBufferInfo = &bufferInfo;

            descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[1].dstSet = m_models[0]->GetMaterial(i).descriptorSet;
            descriptorWrites[1].dstBinding = 1;
            descriptorWrites[1].descriptorCount = 1;
            descriptorWrites[1].pImageInfo = &image_descriptors[0];

            descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[2].dstSet = m_models[0]->GetMaterial(i).descriptorSet;
            descriptorWrites[2].dstBinding = 2;
            descriptorWrites[2].descriptorCount = 1;
            descriptorWrites[2].pImageInfo = &image_descriptors[1];

            descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[3].dstSet = m_models[0]->GetMaterial(i).descriptorSet;
            descriptorWrites[3].dstBinding = 3;
            descriptorWrites[3].descriptorCount = 1;
            descriptorWrites[3].pImageInfo = &image_descriptors[2];

            descriptorWrites[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[4].dstSet = m_models[0]->GetMaterial(i).descriptorSet;
            descriptorWrites[4].dstBinding = 4;
            descriptorWrites[4].descriptorCount = 1;
            descriptorWrites[4].pImageInfo = &image_descriptors[3];

            descriptorWrites[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[5].dstSet = m_models[0]->GetMaterial(i).descriptorSet;
            descriptorWrites[5].dstBinding = 5;
            descriptorWrites[5].descriptorCount = 1;
            descriptorWrites[5].pImageInfo = &image_descriptors[4];

            vkUpdateDescriptorSets(m_device, descriptorWrites.size(), descriptorWrites.data(), 0, nullptr);
        }

        VkPipelineLayoutCreateInfo pipelineLayoutCI{};
        pipelineLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutCI.setLayoutCount = 1;
        pipelineLayoutCI.pSetLayouts = &m_descriptorSetLayouts.scene;
        if (vkCreatePipelineLayout(m_device, &pipelineLayoutCI, nullptr, &m_pipeline_layouts.scene) != VK_SUCCESS) {
            throw std::runtime_error("failed to create pipeline layout!");
        }

        CreateGraphicsPipeline();

        // SetupSkybox
        {
            const std::vector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings = {
                { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, // Environment texture
            };

            // create descriptro set layout 
            VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI{};
            descriptorSetLayoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descriptorSetLayoutCI.pBindings = descriptorSetLayoutBindings.data();
            descriptorSetLayoutCI.bindingCount = descriptorSetLayoutBindings.size();
            if (vkCreateDescriptorSetLayout(m_device, &descriptorSetLayoutCI, nullptr, &m_descriptorSetLayouts.skybox) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create descriptor pool");
            }

            // create pipeline layout
            VkPipelineLayoutCreateInfo pipelineLayoutCI{};
            pipelineLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipelineLayoutCI.setLayoutCount = 1;
            pipelineLayoutCI.pSetLayouts = &m_descriptorSetLayouts.skybox;
            if (vkCreatePipelineLayout(m_device, &pipelineLayoutCI, nullptr, &m_pipeline_layouts.skybox) != VK_SUCCESS) {
                throw std::runtime_error("failed to create pipeline layout!");
            }

            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = m_descriptor_pools.scene;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &m_descriptorSetLayouts.skybox;

            if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptor_sets.skybox) != VK_SUCCESS) {
                throw std::runtime_error("failed to allocate descriptor sets!");
            }

            VkDescriptorBufferInfo buffer_info{};
            buffer_info.buffer = m_ubo.uniformBuffers[0];
            buffer_info.offset = 0;
            buffer_info.range = sizeof(UBO);
            VkDescriptorImageInfo image_info = { hdr->GetSampler(), hdr->GetView(), hdr->GetLayout() };

            std::vector<VkWriteDescriptorSet> write_descriptor_sets;
            write_descriptor_sets.resize(2);
            write_descriptor_sets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_descriptor_sets[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write_descriptor_sets[0].dstSet = m_descriptor_sets.skybox;
            write_descriptor_sets[0].dstBinding = 0;
            write_descriptor_sets[0].descriptorCount = 1;
            write_descriptor_sets[0].pBufferInfo = &buffer_info;

            write_descriptor_sets[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write_descriptor_sets[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write_descriptor_sets[1].dstSet = m_descriptor_sets.skybox;
            write_descriptor_sets[1].dstBinding = 1;
            write_descriptor_sets[1].descriptorCount = 1;
            write_descriptor_sets[1].pImageInfo = &image_info;

            vkUpdateDescriptorSets(m_device, write_descriptor_sets.size(), write_descriptor_sets.data(), 0, nullptr);

            // create pipeline
            {
                auto vert_shader_code = Utils::File::ReadFile("../shaders/skybox/skybox_vert.spv");
                auto frag_shader_code = Utils::File::ReadFile("../shaders/skybox/skybox_frag.spv");

                VkShaderModule vert_shader_module = vkUtilities::CreateShaderModule(vert_shader_code, m_device);
                VkShaderModule frag_shader_module = vkUtilities::CreateShaderModule(frag_shader_code, m_device);

                VkPipelineShaderStageCreateInfo vert_shader_stage_info{};
                vert_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                vert_shader_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
                vert_shader_stage_info.module = vert_shader_module;
                vert_shader_stage_info.pName = "main";

                VkPipelineShaderStageCreateInfo frag_shader_stage_info{};
                frag_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                frag_shader_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                frag_shader_stage_info.module = frag_shader_module;
                frag_shader_stage_info.pName = "main";

                VkPipelineShaderStageCreateInfo shaderStages[] = { vert_shader_stage_info, frag_shader_stage_info };

                VkPipelineVertexInputStateCreateInfo vertex_input_info{};
                vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

                const std::vector<VkVertexInputBindingDescription> vertexInputBindings = {
                    { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX },
                };
                const std::vector<VkVertexInputAttributeDescription> vertexAttributes = {
                    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, // Position
                };

                vertex_input_info.vertexBindingDescriptionCount = 1;
                vertex_input_info.pVertexBindingDescriptions = vertexInputBindings.data();
                vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size());
                vertex_input_info.pVertexAttributeDescriptions = vertexAttributes.data();

                VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
                inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                inputAssembly.primitiveRestartEnable = VK_FALSE;

                VkPipelineViewportStateCreateInfo viewport_state{};
                viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                viewport_state.viewportCount = 1;
                viewport_state.scissorCount = 1;

                VkPipelineRasterizationStateCreateInfo rasterizer{};
                rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                rasterizer.depthClampEnable = VK_FALSE;
                rasterizer.rasterizerDiscardEnable = VK_FALSE;
                rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
                rasterizer.lineWidth = 1.0f;
                rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
                rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                rasterizer.depthBiasEnable = VK_FALSE;

                VkPipelineMultisampleStateCreateInfo multisampleState = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
                //multisampleState.rasterizationSamples = static_cast<VkSampleCountFlagBits>(m_renderTargets[0].samples);
                multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

                VkPipelineMultisampleStateCreateInfo multi_sampling{};
                multi_sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                multi_sampling.sampleShadingEnable = VK_FALSE;
                multi_sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

                VkPipelineDepthStencilStateCreateInfo depthStencilState = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
                depthStencilState.depthTestEnable = VK_FALSE;

                VkPipelineDepthStencilStateCreateInfo depthStencil{};
                depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                depthStencil.depthTestEnable = VK_FALSE;
                depthStencil.depthWriteEnable = VK_FALSE;
                depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                depthStencil.depthBoundsTestEnable = VK_FALSE;
                depthStencil.stencilTestEnable = VK_FALSE;

                VkPipelineColorBlendAttachmentState color_blend_attachment{};
                color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                color_blend_attachment.blendEnable = VK_FALSE;

                VkPipelineColorBlendStateCreateInfo color_blending{};
                color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                color_blending.logicOpEnable = VK_FALSE;
                color_blending.logicOp = VK_LOGIC_OP_COPY;
                color_blending.attachmentCount = 1;
                color_blending.pAttachments = &color_blend_attachment;
                color_blending.blendConstants[0] = 0.0f;
                color_blending.blendConstants[1] = 0.0f;
                color_blending.blendConstants[2] = 0.0f;
                color_blending.blendConstants[3] = 0.0f;

                std::vector<VkDynamicState> dynamic_states = {
                    VK_DYNAMIC_STATE_VIEWPORT,
                    VK_DYNAMIC_STATE_SCISSOR
                };
                VkPipelineDynamicStateCreateInfo dynamic_state{};
                dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
                dynamic_state.pDynamicStates = dynamic_states.data();

                VkGraphicsPipelineCreateInfo pipeline_info{};
                pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                pipeline_info.stageCount = 2;
                pipeline_info.pStages = shaderStages;
                pipeline_info.pVertexInputState = &vertex_input_info;
                pipeline_info.pInputAssemblyState = &inputAssembly;
                pipeline_info.pViewportState = &viewport_state;
                pipeline_info.pRasterizationState = &rasterizer;
                pipeline_info.pMultisampleState = &multi_sampling;
                pipeline_info.pDepthStencilState = &depthStencil;
                pipeline_info.pColorBlendState = &color_blending;
                pipeline_info.pDynamicState = &dynamic_state;
                pipeline_info.layout = m_pipeline_layouts.skybox;
                pipeline_info.renderPass = m_render_pass;
                pipeline_info.subpass = 0;
                pipeline_info.basePipelineHandle = VK_NULL_HANDLE;

                if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipelines.skybox) != VK_SUCCESS) {
                    LOG_ERROR(false, "Failed to create graphics pipeline!");
                }

                vkDestroyShaderModule(m_device, frag_shader_module, nullptr);
                vkDestroyShaderModule(m_device, vert_shader_module, nullptr);
            }
        } // END - SetupSkybox
    }

    void GraphicsDevice::CreateVertexBuffer(VkBuffer& vertex_buffer, VkDeviceMemory& vertex_buffer_memory, uint32_t buffer_size, const Vertex* vertices) {
        VkDeviceSize bufferSize = buffer_size;

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        vkUtilities::CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory, m_physical_device, m_device);

        void* data;
        vkMapMemory(m_device, stagingBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, vertices, (size_t)bufferSize);
        vkUnmapMemory(m_device, stagingBufferMemory);

        vkUtilities::CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertex_buffer, vertex_buffer_memory, m_physical_device, m_device);
        vkUtilities::CopyBuffer(stagingBuffer, vertex_buffer, bufferSize, m_command_pool, m_device, m_graphics_queue);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingBufferMemory, nullptr);
    }
    void GraphicsDevice::CreateIndexBuffer(VkBuffer& index_buffer, VkDeviceMemory& index_buffer_memory, uint32_t buffer_size, const uint32_t* indices) {
        //m_indices_size = indices.size();
        VkDeviceSize bufferSize = buffer_size;

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        vkUtilities::CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory, m_physical_device, m_device);

        void* data;
        vkMapMemory(m_device, stagingBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, indices, (size_t)bufferSize);
        vkUnmapMemory(m_device, stagingBufferMemory);

        vkUtilities::CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, index_buffer, index_buffer_memory, m_physical_device, m_device);

        vkUtilities::CopyBuffer(stagingBuffer, index_buffer, bufferSize, m_command_pool, m_device, m_graphics_queue);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingBufferMemory, nullptr);
    }

    void GraphicsDevice::CreateUniformBuffer() {
        VkDeviceSize buffer_size = sizeof(UBO);
        m_ubo.uniformBuffers.resize(m_render_ahead);
        m_ubo.uniformBuffersMemory.resize(m_render_ahead);
        m_ubo.uniformBuffersMapped.resize(m_render_ahead);
        for (int i = 0; i < m_ubo.uniformBuffers.size(); i++) {
            vkUtilities::CreateBuffer(buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m_ubo.uniformBuffers[i],
                m_ubo.uniformBuffersMemory[i], m_physical_device, m_device);

            vkMapMemory(m_device, m_ubo.uniformBuffersMemory[i], 0, buffer_size, 0, &m_ubo.uniformBuffersMapped[i]);
        }
    }

    void GraphicsDevice::CreateGraphicsPipeline() {
        // Create Graphics Pipeline
        auto vert_shader_code = Utils::File::ReadFile("../shaders/pbr/pbr_vert.spv");
        auto frag_shader_code = Utils::File::ReadFile("../shaders/pbr/pbr_frag.spv");

        VkShaderModule vert_shader_module = vkUtilities::CreateShaderModule(vert_shader_code, m_device);
        VkShaderModule frag_shader_module = vkUtilities::CreateShaderModule(frag_shader_code, m_device);

        VkPipelineShaderStageCreateInfo vert_shader_stage_info{};
        vert_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vert_shader_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vert_shader_stage_info.module = vert_shader_module;
        vert_shader_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo frag_shader_stage_info{};
        frag_shader_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        frag_shader_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        frag_shader_stage_info.module = frag_shader_module;
        frag_shader_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = { vert_shader_stage_info, frag_shader_stage_info };

        VkPipelineVertexInputStateCreateInfo vertex_input_info{};
        vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        //auto bindingDescription = Vertex::getBindingDescription();
        //auto attributeDescriptions = Vertex::getAttributeDescriptions();
        VkVertexInputBindingDescription vertex_input_binding = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
        std::vector<VkVertexInputAttributeDescription> vertexInputAttributes = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3 },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 6 },
            { 3, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 8 },
            { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 10 }
            //{ 5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 14 },
            //{ 6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 18 }
        };

        vertex_input_info.vertexBindingDescriptionCount = 1;
        vertex_input_info.pVertexBindingDescriptions = &vertex_input_binding;
        vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributes.size());
        vertex_input_info.pVertexAttributeDescriptions = vertexInputAttributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        VkPipelineMultisampleStateCreateInfo multi_sampling{};
        multi_sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multi_sampling.sampleShadingEnable = VK_FALSE;
        multi_sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState color_blend_attachment{};
        color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        color_blend_attachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo color_blending{};
        color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending.logicOpEnable = VK_FALSE;
        color_blending.logicOp = VK_LOGIC_OP_COPY;
        color_blending.attachmentCount = 1;
        color_blending.pAttachments = &color_blend_attachment;
        color_blending.blendConstants[0] = 0.0f;
        color_blending.blendConstants[1] = 0.0f;
        color_blending.blendConstants[2] = 0.0f;
        color_blending.blendConstants[3] = 0.0f;

        std::vector<VkDynamicState> dynamic_states = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamic_state{};
        dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
        dynamic_state.pDynamicStates = dynamic_states.data();

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = shaderStages;
        pipeline_info.pVertexInputState = &vertex_input_info;
        pipeline_info.pInputAssemblyState = &inputAssembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterizer;
        pipeline_info.pMultisampleState = &multi_sampling;
        pipeline_info.pDepthStencilState = &depthStencil;
        pipeline_info.pColorBlendState = &color_blending;
        pipeline_info.pDynamicState = &dynamic_state;
        pipeline_info.layout = m_pipeline_layouts.scene;
        pipeline_info.renderPass = m_render_pass;
        pipeline_info.subpass = 0;
        pipeline_info.basePipelineHandle = VK_NULL_HANDLE;

        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_pipelines.scene) != VK_SUCCESS) {
            LOG_ERROR(false, "Failed to create graphics pipeline!");
        }

        vkDestroyShaderModule(m_device, frag_shader_module, nullptr);
        vkDestroyShaderModule(m_device, vert_shader_module, nullptr);
    }

    void GraphicsDevice::Draw(Camera* camera) {
        vkWaitForFences(m_device, 1, &m_wait_fences[m_current_frame_index], VK_TRUE, UINT64_MAX);

        if (m_framebuffer_resized) {
            RecreateSwapchain();
            m_framebuffer_resized = false;
            return;
        }

        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain->GetSwapchain(), UINT64_MAX, m_render_complete_semaphores[m_current_frame_index], VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain();
            m_framebuffer_resized = false;
            return;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            LOG_ERROR(false, "Failed to acquire swap chain image!");
        }

        // Updating uniform buffers
        {
            UBO ubo{};
            ubo.model = glm::mat4(1.0);
            ubo.view = camera->GetView();
            ubo.proj = camera->GetProjection();

            memcpy(m_ubo.uniformBuffersMapped[m_current_frame_index], &ubo, sizeof(ubo));
        }

        vkResetFences(m_device, 1, &m_wait_fences[m_current_frame_index]);

        vkResetCommandBuffer(m_command_buffers[m_current_frame_index], /*VkCommandBufferResetFlagBits*/ 0);
        RecordCommandBuffer(m_command_buffers[m_current_frame_index], imageIndex);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = { m_render_complete_semaphores[m_current_frame_index] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;

        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_command_buffers[m_current_frame_index];

        VkSemaphore signalSemaphores[] = { m_present_complete_semaphores[m_current_frame_index] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(m_graphics_queue, 1, &submitInfo, m_wait_fences[m_current_frame_index]) != VK_SUCCESS) {
            LOG_ERROR(false, "failed to submit draw command buffer!");
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;

        VkSwapchainKHR swapChains[] = { m_swapchain->GetSwapchain()};

        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        

        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(m_present_queue, &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebuffer_resized) {
            m_framebuffer_resized = false;
            CleanUpSwapchain();
        }
        else if (result != VK_SUCCESS) {
            LOG_ERROR(false, "failed to present swap chain image!");
        }
        m_current_frame_index = (m_current_frame_index + 1) % m_render_ahead;
    }

    void GraphicsDevice::RecordCommandBuffer(VkCommandBuffer command_buffer, uint32_t image_index) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command_buffer, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("failed to begin recording command buffer!");
        }

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_render_pass;
        renderPassInfo.framebuffer = m_framebuffers[image_index];
        renderPassInfo.renderArea.offset = { 0, 0 };
        renderPassInfo.renderArea.extent = m_swapchain->GetExtent();

        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
        clearValues[1].depthStencil = { 1.0f, 0 };

        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(command_buffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float)m_swapchain->GetExtentWidth();
        viewport.height = (float)m_swapchain->GetExtentHeight();
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = m_swapchain->GetExtent();
        vkCmdSetScissor(command_buffer, 0, 1, &scissor);

        bool skybox = true;
        if (skybox) {
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layouts.skybox, 0, 1, &m_descriptor_sets.skybox, 0, nullptr);
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines.skybox);
            VkBuffer vertexBuffers[] = { m_models[1]->m_vertices.buffer };
            VkDeviceSize offsets[] = { 0 };
            vkCmdBindVertexBuffers(command_buffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(command_buffer, m_models[1]->m_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            //models.skybox.draw(currentCB);
            for (auto& node : m_models[1]->GetNodes()) {
                DrawNodeSkybox(node, command_buffer);
            }
        }

        //vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &m_descriptor_sets.scene[m_current_frame_index], 0, nullptr);
        if(false){
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines.scene);
            VkBuffer vertexBuffers[] = { m_models[0]->m_vertices.buffer };
            VkDeviceSize offsets[] = { 0 };
            vkCmdBindVertexBuffers(command_buffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(command_buffer, m_models[0]->m_indices.buffer, 0, VK_INDEX_TYPE_UINT32);

            for (auto& node : m_models[0]->GetNodes()) {
            	DrawNode(node, command_buffer);
            }
        }

        vkCmdEndRenderPass(command_buffer);

        if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to record command buffer!");
        }
    }

    void GraphicsDevice::DrawNode(Node* node, VkCommandBuffer commandBuffer) {
        if (node->mesh) {
            for (Primitive* primitive : node->mesh->primitives) {
                uint32_t index = primitive->material_index > -1 ? primitive->material_index : 0;
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layouts.scene, 0, 1, 
                    &m_models[0]->GetMaterial(index).descriptorSet, 0, NULL);
                vkCmdDrawIndexed(commandBuffer, primitive->index_count, 1, primitive->first_index, 0, 0);
            }
        }
        for (auto& child : node->children) {
            DrawNode(child, commandBuffer);
        }
    }

    void GraphicsDevice::DrawNodeSkybox(Node* node, VkCommandBuffer commandBuffer) {
        if (node->mesh) {
            for (Primitive* primitive : node->mesh->primitives) {
                uint32_t index = primitive->material_index > -1 ? primitive->material_index : 0;
                //vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layouts.skybox, 0, 1,
                //    &m_models[0]->GetMaterial(index).descriptorSet, 0, NULL);
                vkCmdDrawIndexed(commandBuffer, primitive->index_count, 1, primitive->first_index, 0, 0);
            }
        }
        for (auto& child : node->children) {
            DrawNodeSkybox(child, commandBuffer);
        }
    }

    void GraphicsDevice::CleanUpSwapchain() {
        vkDestroyImageView(m_device, m_depth_image_view, nullptr);
        vkDestroyImage(m_device, m_depth_image, nullptr);
        vkFreeMemory(m_device, m_depth_image_memory, nullptr);
        for (auto framebuffer : m_framebuffers) {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
        m_swapchain->Destroy();
    }

    void GraphicsDevice::CleanUp(const Config& config)
    {
        glfwWaitEvents();
        vkDeviceWaitIdle(m_device);
        CleanUpSwapchain();
        //vkDestroySampler(m_device, m_texture_sampler, nullptr);
        //vkDestroyImageView(m_device, m_texture_image_view, nullptr);
        //vkDestroyImage(m_device, m_texture_image, nullptr);
        //vkFreeMemory(m_device, m_texture_image_memory, nullptr);
        for (size_t i = 0; i < 1; i++) {
            vkDestroyBuffer(m_device, m_uniform_buffers[i], nullptr);
            vkFreeMemory(m_device, m_uniform_buffers_memory[i], nullptr);
        }
        //vkDestroyDescriptorPool(m_device, m_descriptor_pool, nullptr);
        //vkDestroyDescriptorSetLayout(m_device, m_descriptor_set_layout, nullptr);
        //vkDestroyBuffer(m_device, m_index_buffer, nullptr);
        //vkFreeMemory(m_device, m_index_buffer_memory, nullptr);
        //vkDestroyBuffer(m_device, m_vertex_buffer, nullptr);
        //vkFreeMemory(m_device, m_vertex_buffer_memory, nullptr);
        //vkDestroyPipeline(m_device, m_graphics_pipeline, nullptr);
        //vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
        vkDestroyRenderPass(m_device, m_render_pass, nullptr);
        for (size_t i = 0; i < m_render_ahead; i++) {
            vkDestroySemaphore(m_device, m_render_complete_semaphores[i], nullptr);
            vkDestroySemaphore(m_device, m_present_complete_semaphores[i], nullptr);
            vkDestroyFence(m_device, m_wait_fences[i], nullptr);
        }
        vkDestroyCommandPool(m_device, m_command_pool, nullptr);
        vkDestroyDevice(m_device, nullptr);
        if (config.enable_validation_layers)
            vkUtilities::DestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger, nullptr);
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        vkDestroyInstance(m_instance, nullptr);
        m_window->DestroyWindow();
        glfwTerminate();
    }

    void GraphicsDevice::RecreateSwapchain() {
        int width = 0, height = 0;
        glfwGetFramebufferSize(m_window->window(), &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(m_window->window(), &width, &height);
            glfwWaitEvents();
        }
        vkDeviceWaitIdle(m_device);
        CleanUpSwapchain();
        
        // Create swap chain
        m_swapchain = std::make_unique<Swapchain>(this);
        m_swapchain->Initialize();

        // === Create Depth Resource ===
        VkFormat depthFormat = vkUtilities::FindDepthFormat(m_physical_device);
        vkUtilities::CreateImage(m_swapchain->GetExtentWidth(), m_swapchain->GetExtentHeight(), m_device, m_physical_device, depthFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_depth_image, m_depth_image_memory, 1, 1);
        m_depth_image_view = vkUtilities::CreateImageView(m_depth_image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, m_device, 1, 0, 1);

        // === Create Framebuffers ===
        {
            m_framebuffers.resize(m_swapchain->GetSwapchainImageViews().size());

            for (size_t i = 0; i < m_swapchain->GetSwapchainImageViews().size(); i++) {
                std::array<VkImageView, 2> attachments = {
                    m_swapchain->GetSwapchainImageView(i),
                    m_depth_image_view
                };

                VkFramebufferCreateInfo framebuffer_info{};
                framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebuffer_info.renderPass = m_render_pass;
                framebuffer_info.attachmentCount = static_cast<uint32_t>(attachments.size());
                framebuffer_info.pAttachments = attachments.data();
                framebuffer_info.width = m_swapchain->GetExtentWidth();
                framebuffer_info.height = m_swapchain->GetExtentHeight();
                framebuffer_info.layers = 1;

                if (vkCreateFramebuffer(m_device, &framebuffer_info, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
                    LOG_ERROR(false, "Failed to create framebuffer!");
                }
            }
        }
    }
}