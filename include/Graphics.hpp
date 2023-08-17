#include "VulkanUtilities.hpp"
#include <vector>
namespace Diffuse {
	struct Config {
		bool enable_validation_layers = false;
		const std::vector<const char*> validation_layers = {
			"VK_LAYER_KHRONOS_validation"
		};
		const std::vector<const char*> device_extensions = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME
		};
	};
	class Graphics {
	public:
		bool Init(const Config& config);
		void Draw();
	private:
		VkDevice m_device;
		GLFWwindow* m_window;
		VkInstance m_instance;
		VkSurfaceKHR m_surface;
		VkQueue m_present_queue;
		VkQueue m_graphics_queue;
		VkRenderPass m_render_pass;
		VkSwapchainKHR m_swap_chain;
		VkCommandPool m_command_pool;
		VkExtent2D m_swap_chain_extent;
		VkPipeline m_graphics_pipeline;
		VkCommandBuffer m_command_buffer;
		VkFormat m_swap_chain_image_format;
		VkPipelineLayout m_pipeline_layout;
		std::vector<VkImage> m_swap_chain_images;
		VkDebugUtilsMessengerEXT m_debug_messenger;
		std::vector<VkImageView> m_swap_chain_image_views;
		std::vector<VkFramebuffer> m_swap_chain_framebuffers; 
		VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;

		int m_width = 1200;
		int m_height = 720;
	};
}