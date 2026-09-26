#include "libs/audio3d.h"

#include "common/assert.h"
#include "common/threads.h"
#include "kernel/semaphore.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <atomic>

namespace Libs::Audio {

namespace Audio3d {

LIB_NAME("Audio3d", "Audio3d");

namespace Semaphore = LibKernel::Semaphore;

struct Audio3dOpenParameters {
	size_t   size        = 0x20;
	uint32_t granularity = 256;
	uint32_t rate        = 0;
	uint32_t max_objects = 512;
	uint32_t queue_depth = 2;
	uint32_t buffer_mode = 2;
	uint32_t pad         = 0;
	// uint32_t num_beds;
};

static_assert(sizeof(Audio3dOpenParameters) == 0x20);

struct Audio3dData {
	enum class State { Empty, Ready, Play };

	std::atomic<State> state = State::Empty;
};

struct Audio3dInternal {
	Audio3dData*          data                        = nullptr;
	Common::Mutex*        data_mutex                  = nullptr;
	uint64_t              data_delay                  = 0;
	Semaphore::KernelSema playback_sema               = nullptr;
	Audio3dOpenParameters params                      = {};
	int                   user_id                     = 0;
	float                 late_reverb_level           = 0.0f;
	float                 downmix_spread_radius       = 2.0f;
	int                   downmix_spread_height_aware = 0;
	uint32_t              data_index                  = 0;
	uint32_t              next_object_id              = 1;
	uint32_t              reserved_objects            = 0;
	bool                  used                        = false;
	std::atomic_bool      playback_finished           = false;
};

constexpr uint32_t MAX_PORTS = 4;

static Audio3dInternal g_ports[MAX_PORTS] = {};

static void playback_simulate(void* arg) {
	auto* port = static_cast<Audio3dInternal*>(arg);
	EXIT_IF(port == nullptr);
	EXIT_IF(port->data_mutex == nullptr);
	EXIT_IF(port->data == nullptr);

	for (;;) {
		int result = Semaphore::KernelWaitSema(port->playback_sema, 1, nullptr);

		if (result != OK) {
			break;
		}

		Audio3dData* play_data = nullptr;

		port->data_mutex->Lock();
		{
			for (uint32_t i = 0; i < port->params.queue_depth; i++) {
				uint32_t index = (port->data_index + i) % port->params.queue_depth;

				if (port->data[index].state == Audio3dData::State::Play) {
					play_data = &port->data[index];
					break;
				}
			}
		}
		port->data_mutex->Unlock();

		EXIT_IF(play_data == nullptr);

		if (play_data != nullptr) {
			// TODO(): Audio output is not yet implemented, so simulate audio delay
			Common::Thread::SleepMicro(port->data_delay);
			play_data->state = Audio3dData::State::Empty;
		}
	}

	port->playback_finished = true;
}

int KYTY_SYSV_ABI Audio3dInitialize(int64_t reserved) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(reserved != 0);

	return OK;
}

void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(sizeof(Audio3dOpenParameters) != 0x20);

	*p = Audio3dOpenParameters();
}

int KYTY_SYSV_ABI Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters,
                                  uint32_t* id) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(parameters == nullptr);
	EXIT_NOT_IMPLEMENTED(id == nullptr);
	LOGF("\t parameter_size = 0x%zx\n", parameters->size);
	// Jagged Alliance 3 uses a 0x28-byte block with the same timing/queue prefix.
	// The playback simulator consumes only that prefix; leave extension fields unread.
	EXIT_NOT_IMPLEMENTED(parameters->size != sizeof(Audio3dOpenParameters) &&
	                     parameters->size != 0x28);

	LOGF("\t user_id     = %d\n"
	     "\t granularity = %u\n"
	     "\t rate        = %u\n"
	     "\t max_objects = %u\n"
	     "\t queue_depth = %u\n"
	     "\t buffer_mode = %u\n",
	     user_id, parameters->granularity, parameters->rate, parameters->max_objects,
	     parameters->queue_depth, parameters->buffer_mode);

	EXIT_NOT_IMPLEMENTED(parameters->buffer_mode != 2);
	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 1);

	uint32_t port = 0;
	for (; port < MAX_PORTS; port++) {
		if (!g_ports[port].used) {
			break;
		}
	}

	EXIT_NOT_IMPLEMENTED(port >= MAX_PORTS);

	g_ports[port].user_id = user_id;
	g_ports[port].params  = *parameters;
	g_ports[port].used    = true;

	EXIT_IF(g_ports[port].data != nullptr);
	EXIT_IF(g_ports[port].data_mutex != nullptr);
	EXIT_IF(g_ports[port].playback_sema != nullptr);

	g_ports[port].data       = new Audio3dData[parameters->queue_depth];
	g_ports[port].data_index = 0;
	g_ports[port].data_mutex = new Common::Mutex;
	g_ports[port].data_delay = (1000000 * static_cast<uint64_t>(parameters->granularity)) / 48000;

	for (uint32_t d = 0; d < parameters->queue_depth; d++) {
		g_ports[port].data[d].state = Audio3dData::State::Empty;
	}

	int result = Semaphore::KernelCreateSema(&g_ports[port].playback_sema, "audio3d_play", 0x01, 0,
	                                         static_cast<int>(parameters->queue_depth), nullptr);
	EXIT_NOT_IMPLEMENTED(result != OK);

	g_ports[port].playback_finished = false;
	Common::Thread playback_thread(playback_simulate, &g_ports[port]);
	playback_thread.Detach();

	*id = port;

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id,
                                          const void* attribute, size_t attribute_size) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(attribute == nullptr);

	LOGF("\t attribute_id = 0x%" PRIx32 "\n", attribute_id);

	switch (attribute_id) {
		case 0x10001:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].late_reverb_level = *static_cast<const float*>(attribute);
			LOGF("\t late_reverb_level = %f\n", g_ports[port_id].late_reverb_level);
			break;
		case 0x10002:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].downmix_spread_radius = *static_cast<const float*>(attribute);
			LOGF("\t downmix_spread_radius = %f\n", g_ports[port_id].downmix_spread_radius);
			break;
		case 0x10003:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].downmix_spread_height_aware = *static_cast<const int*>(attribute);
			LOGF("\t downmix_spread_height_aware = %d\n",
			     g_ports[port_id].downmix_spread_height_aware);
			break;
		default: EXIT("unknown attribute: 0x%" PRIx32 "\n", attribute_id);
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level,
                                           uint32_t* queue_available) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(queue_level == nullptr && queue_available == nullptr);

	auto* port = &g_ports[port_id];

	uint32_t empty_num = 0;

	port->data_mutex->Lock();
	{
		for (uint32_t i = 0; i < port->params.queue_depth; i++) {
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Empty) {
				empty_num++;
			} else {
				break;
			}
		}
	}
	port->data_mutex->Unlock();

	EXIT_IF(empty_num > port->params.queue_depth);

	LOGF("\t queue_available = %u\n", empty_num);

	if (queue_level != nullptr) {
		*queue_level = port->params.queue_depth - empty_num;
	}
	if (queue_available != nullptr) {
		*queue_available = empty_num;
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortAdvance(uint32_t port_id) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);

	auto* port = &g_ports[port_id];

	port->data_mutex->Lock();
	{
		uint32_t current_index = port->data_index;
		uint32_t next_index    = (current_index + 1) % port->params.queue_depth;

		if (port->data[current_index].state == Audio3dData::State::Empty) {
			port->data[current_index].state = Audio3dData::State::Ready;
		}

		EXIT_NOT_IMPLEMENTED(port->data[current_index].state != Audio3dData::State::Ready);

		port->data_index = next_index;

		LOGF("\t %u -> %u\n", current_index, next_index);
	}
	port->data_mutex->Unlock();

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortPush(uint32_t port_id, uint32_t blocking) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);

	auto* port = &g_ports[port_id];

	EXIT_NOT_IMPLEMENTED(blocking != 1);

	LOGF("\t blocking = %u\n", blocking);

	int          data_num   = 0;
	Audio3dData* first_data = nullptr;

	port->data_mutex->Lock();
	{
		first_data = port->data + port->data_index;

		for (uint32_t i = 0; i < port->params.queue_depth; i++) {
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Ready) {
				port->data[index].state = Audio3dData::State::Play;
				data_num++;
			}
		}
	}
	port->data_mutex->Unlock();

	LOGF("\t push num = %d\n", data_num);

	if (data_num > 0) {
		Semaphore::KernelSignalSema(port->playback_sema, data_num);

		if (blocking == 1) {
			auto wait_time = port->data_delay / 8;
			while (first_data->state != Audio3dData::State::Empty) {
				Common::Thread::SleepMicro(wait_time);
			}
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dObjectReserve(uint32_t port_id, uint32_t* object_id) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(object_id == nullptr);

	auto* port = &g_ports[port_id];
	EXIT_NOT_IMPLEMENTED(port->reserved_objects >= port->params.max_objects);

	uint32_t id = port->next_object_id++;
	if (id == 0u || id == 0xFFFFFFFFu) {
		id = port->next_object_id++;
	}

	port->reserved_objects++;
	*object_id = id;

	if (port->reserved_objects == 1 || port->reserved_objects == port->params.max_objects) {
		LOGF("\t object_id = %u (reserved %u / %u)\n", id, port->reserved_objects,
		     port->params.max_objects);
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dObjectSetAttributes(uint32_t port_id, uint32_t object_id,
                                             uint64_t num_attributes, const void* attribute_array) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(num_attributes > 0 && attribute_array == nullptr);

	// 3D object pose/PCM attributes are unused while output is simulated at port granularity.
	static std::atomic_bool logged {false};
	if (!logged.exchange(true, std::memory_order_relaxed)) {
		LOGF("\t object_id = %u, num_attributes = %" PRIu64 "\n", object_id, num_attributes);
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dObjectUnreserve(uint32_t port_id, uint32_t object_id) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(object_id == 0u || object_id == 0xFFFFFFFFu);

	auto* port = &g_ports[port_id];
	if (port->reserved_objects > 0) {
		port->reserved_objects--;
	}

	return OK;
}

} // namespace Audio3d

} // namespace Libs::Audio
