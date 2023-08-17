#include "Application.hpp"

#include <iostream>

namespace Diffuse {
	void Application::Init() {
		m_graphics = new Graphics();
		m_config.enable_validation_layers = false;
		if (m_graphics->Init(m_config)) {
			std::cout << "SUCCESS";
		}
		else {
			std::cout << "Failure setting up Vulkan";
		}
	}
	void Application::Update()
	{
		while (!glfwWindowShouldClose(m_graphics->GetWindow())) {
			glfwPollEvents();
			m_graphics->Draw();
		}
	}
}