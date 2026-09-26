#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		EXIT("Vulkan getSemaphoreCounterValue failed: %s (%d), known GPU tick=%" PRIu64 "\n",
		     vk::to_string(result).c_str(), static_cast<int>(result), KnownGpuTick());
	}

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	// Some Windows GPU drivers treat UINT64_MAX as a signed -1 ns timeout and return
	// immediately with TIMEOUT/UNKNOWN. Wait in finite slices until the GPU tick arrives.
	constexpr uint64_t kWaitNs = 1'000'000'000ull;
	for (;;) {
		const auto result = m_graphics.device.waitSemaphores(&wait_info, kWaitNs);
		if (result == vk::Result::eSuccess || result == vk::Result::eTimeout) {
			Refresh();
			if (IsFree(tick)) {
				return;
			}
			continue;
		}
		EXIT("Vulkan waitSemaphores failed: %s (%d), requested tick=%" PRIu64
		     ", known GPU tick=%" PRIu64 ", next tick=%" PRIu64 "\n",
		     vk::to_string(result).c_str(), static_cast<int>(result), tick, KnownGpuTick(),
		     CurrentTick());
	}
}

} // namespace Libs::Graphics
