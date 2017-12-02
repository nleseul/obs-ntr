#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <turbojpeg.h>

#include <WinSock2.h>

// Extracted from ns.h in the NTR source.
enum ntr_command_type
{
	NS_TYPE_NORMAL,
	NS_TYPE_BIGDATA
};

enum ntr_command
{
	NS_CMD_REMOTEPLAY = 901
};


enum ntr_screen
{
	SCREEN_BOTTOM,
	SCREEN_TOP,

	SCREEN_COUNT
};

const int SCREEN_WIDTH[SCREEN_COUNT] =
{
	320, 
	400
};

const int SCREEN_HEIGHT[SCREEN_COUNT] =
{
	240,
	240
};

struct ntr_command_packet
{
	int magic_number;
	int sequence;
	enum ntr_command_type type;
	enum ntr_command command;
	int args[4];

	unsigned char padding[52]; // Pad to 84 bytes
};

#define DATA_PACKET_DATA_SIZE 1444
#define DATA_PACKET_MAX_COUNT 64
struct ntr_data_packet
{
	unsigned char id;
	unsigned char is_top : 1;
	unsigned char flags_pad : 3;
	unsigned char is_last : 1;
	unsigned char flags_pad2 : 3;
	unsigned char format;
	unsigned char order;

	unsigned char data[DATA_PACKET_DATA_SIZE];
};

struct ntr_connection_setup
{
	struct dstr ip_address;
	int quality;
	int priority_factor;
	int qos;
	enum ntr_screen priority_screen;
};

struct ntr_frame_data
{
	unsigned char is_top;
	unsigned char id;
	unsigned char packet_count;
	unsigned char expected_packet_count;
	unsigned char finished;
	int last_packet_data_size;

	uint64_t time_started;

	unsigned char *frame_data;
};

#define CONCURRENT_FRAMES 4
struct ntr_connection_data
{
	pthread_t net_thread;
	pthread_mutex_t buffer_mutex[SCREEN_COUNT];

	SOCKET data_socket;
	bool is_connected;

	struct ntr_connection_setup connection_setup;

	tjhandle decompressor_handle;
	unsigned char *uncompressed_buffer[SCREEN_COUNT];
	int last_frame_id[SCREEN_COUNT];

	struct ntr_frame_data frames[CONCURRENT_FRAMES];

	int frames_processed;
	int frames_dumped;
	uint64_t last_stat_time;
};

struct ntr_data
{
	obs_source_t *source;

	bool owns_connection;

	bool pending_connect;

	enum ntr_screen screen;
	int last_frame_id;

	struct ntr_connection_setup connection_setup;

	gs_texture_t *texture;
};



void *obs_ntr_net_thread_run(void *data)
{
	struct ntr_connection_data *connection_data = data;

	struct sockaddr_in client_address;
	client_address.sin_family = AF_INET;
	client_address.sin_addr.s_addr = htons(INADDR_ANY);
	client_address.sin_port = 0;

	struct sockaddr_in server_address;
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = inet_addr(connection_data->connection_setup.ip_address.array);
	server_address.sin_port = htons(8000);


	SOCKET command_socket = socket(AF_INET, SOCK_STREAM, 0);
	int bind_result = bind(command_socket, (struct sockaddr *)&client_address, sizeof(struct sockaddr_in));
	int connect_result = connect(command_socket, (struct sockaddr *)&server_address, sizeof(struct sockaddr_in));

	struct ntr_command_packet start_command;
	start_command.magic_number = 0x12345678;
	start_command.sequence = 1;
	start_command.type = NS_TYPE_NORMAL;
	start_command.command = NS_CMD_REMOTEPLAY;
	start_command.args[0] = (connection_data->connection_setup.priority_screen << 8) | connection_data->connection_setup.priority_factor;
	start_command.args[1] = connection_data->connection_setup.quality;
	start_command.args[2] = connection_data->connection_setup.qos * 1024 * 1024 / 8;

	int packet_size = sizeof(struct ntr_command_packet);
	send(command_socket, (const char *)&start_command, packet_size, 0);
	closesocket(command_socket);

	struct sockaddr_in server_address_data;
	server_address_data.sin_family = AF_INET;
	server_address_data.sin_addr.s_addr = htonl(INADDR_ANY);
	server_address_data.sin_port = htons(8001);

	connection_data->data_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	int bind_data_result = bind(connection_data->data_socket, (struct sockaddr *)&server_address_data, sizeof(struct sockaddr_in));


	int buffer_size = 8 * 1024 * 1024;
	struct timeval timeout;
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;
	setsockopt(connection_data->data_socket, SOL_SOCKET, SO_RCVBUF, (char *)&buffer_size, sizeof(int));
	setsockopt(connection_data->data_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(struct timeval));

	connection_data->is_connected = (bind_result == 0 && connect_result >= 0 && bind_data_result == 0);

	connection_data->frames_processed = 0;
	connection_data->frames_dumped = 0;
	connection_data->last_stat_time = obs_get_video_frame_time();

	while (connection_data->is_connected)
	{
		if (connection_data->frames_processed >= 100)
		{
			uint64_t now = obs_get_video_frame_time();

			uint64_t elapsed_ms = (now - connection_data->last_stat_time) / 1000000;
			float elapsed_seconds = (float)(elapsed_ms) / 1000.0f;
			float fps = (connection_data->frames_processed - connection_data->frames_dumped) / elapsed_seconds;

			blog(LOG_DEBUG, "obs-ntr: Dropped %d/%d frames; effective fps=%f", connection_data->frames_dumped, connection_data->frames_processed, fps);

			connection_data->frames_processed = 0;
			connection_data->frames_dumped = 0;
			connection_data->last_stat_time = now;
		}

		struct sockaddr_in from_address;
		int from_address_length = sizeof(struct sockaddr_in);
		struct ntr_data_packet packet;
		int receive_result = recvfrom(connection_data->data_socket, (char *)&packet, sizeof(struct ntr_data_packet), 0, (struct sockaddr *)&from_address, &from_address_length);

		if (receive_result > 0)
		{
			struct ntr_frame_data *active_frame = NULL;

			for (int frame_index = 0; frame_index < CONCURRENT_FRAMES; frame_index++)
			{
				if ((connection_data->frames[frame_index].id == packet.id && connection_data->frames[frame_index].is_top == packet.is_top) ||
					connection_data->frames[frame_index].finished)
				{
					active_frame = &connection_data->frames[frame_index];
					break;
				}
			}

			if (active_frame == NULL)
			{
				int oldest_index = 0;
				uint64_t oldest_time = UINT64_MAX;
				for (int frame_index = 0; frame_index < CONCURRENT_FRAMES; frame_index++)
				{
					if (connection_data->frames[frame_index].time_started < oldest_time)
					{
						oldest_time = connection_data->frames[frame_index].time_started;
						oldest_index = frame_index;
					}
				}

				active_frame = &connection_data->frames[oldest_index];
			}

			assert(active_frame != NULL);

			if (active_frame->id != packet.id || active_frame->is_top != packet.is_top || active_frame->finished)
			{
				if (!active_frame->finished)
				{
					/*blog(LOG_DEBUG, "Dumping frame %d (%d/%d) for frame %d (%d)", active_frame->id,
						active_frame->packet_count, active_frame->expected_packet_count,
						packet.id, packet.is_top);*/

					connection_data->frames_processed++;
					connection_data->frames_dumped++;
				}

				active_frame->is_top = packet.is_top;
				active_frame->id = packet.id;
				active_frame->expected_packet_count = 0;
				active_frame->packet_count = 0;
				active_frame->last_packet_data_size = 0;
				active_frame->finished = false;
				active_frame->time_started = obs_get_video_frame_time();
			}

			if (packet.is_last)
			{
				active_frame->expected_packet_count = packet.order + 1;
				active_frame->last_packet_data_size = receive_result - 4;

				memcpy(active_frame->frame_data + (DATA_PACKET_DATA_SIZE * packet.order), packet.data,
					active_frame->last_packet_data_size);
			}
			else
			{
				memcpy(active_frame->frame_data + (DATA_PACKET_DATA_SIZE * packet.order), packet.data, DATA_PACKET_DATA_SIZE);
			}

			active_frame->packet_count++;

			if (active_frame->expected_packet_count > 0 && active_frame->packet_count >= active_frame->expected_packet_count)
			{
				//blog(LOG_DEBUG, "Finishing frame %d with %d/%d packets", active_frame->id, active_frame->packet_count, active_frame->expected_packet_count);

				active_frame->finished = true;

				connection_data->frames_processed++;

				pthread_mutex_lock(&connection_data->buffer_mutex[packet.is_top]);
				int decompress_result = tjDecompress2(connection_data->decompressor_handle, active_frame->frame_data,
					(active_frame->expected_packet_count - 1) * DATA_PACKET_DATA_SIZE + active_frame->last_packet_data_size,
					connection_data->uncompressed_buffer[packet.is_top], SCREEN_HEIGHT[packet.is_top], SCREEN_HEIGHT[packet.is_top] * 4,
					SCREEN_WIDTH[packet.is_top], TJPF_RGBA, 0);

				connection_data->last_frame_id[packet.is_top] = packet.id;

				pthread_mutex_unlock(&connection_data->buffer_mutex[packet.is_top]);
			}
		}
		else
		{
			sched_yield();
		}
	}

	return 0;
}


static struct ntr_connection_data *shared_connection_data = NULL;

void obs_ntr_connection_create(struct ntr_connection_setup *connection_setup)
{
	struct ntr_connection_data *temp_connection_data = bzalloc(sizeof(struct ntr_connection_data));

	dstr_copy_dstr(&temp_connection_data->connection_setup.ip_address, &connection_setup->ip_address);
	temp_connection_data->connection_setup.quality = connection_setup->quality;
	temp_connection_data->connection_setup.qos = connection_setup->qos;
	temp_connection_data->connection_setup.priority_factor = connection_setup->priority_factor;
	temp_connection_data->connection_setup.priority_screen = connection_setup->priority_screen;

	temp_connection_data->decompressor_handle = tjInitDecompress();

	for (int screen_index = 0; screen_index < SCREEN_COUNT; screen_index++)
	{
		temp_connection_data->uncompressed_buffer[screen_index] = bzalloc(SCREEN_WIDTH[screen_index] * SCREEN_HEIGHT[screen_index] * 4);

		pthread_mutex_init_value(&temp_connection_data->buffer_mutex[screen_index]);
		pthread_mutex_init(&temp_connection_data->buffer_mutex[screen_index], NULL);
	}

	for (int frame_index = 0; frame_index < CONCURRENT_FRAMES; frame_index++)
	{
		temp_connection_data->frames[frame_index].id = 0;
		temp_connection_data->frames[frame_index].packet_count = 0;
		temp_connection_data->frames[frame_index].expected_packet_count = 0;
		temp_connection_data->frames[frame_index].last_packet_data_size = 0;
		temp_connection_data->frames[frame_index].finished = 0;
		temp_connection_data->frames[frame_index].time_started = 0;

		temp_connection_data->frames[frame_index].frame_data = bzalloc(DATA_PACKET_DATA_SIZE * DATA_PACKET_MAX_COUNT);
	}


	shared_connection_data = temp_connection_data;

	struct sched_param thread_schedparam;
	thread_schedparam.sched_priority = 10;

	pthread_attr_t *thread_settings = NULL;
	pthread_attr_init(thread_settings);
	pthread_attr_setschedparam(thread_settings, &thread_schedparam);

	pthread_create(&shared_connection_data->net_thread, thread_settings, obs_ntr_net_thread_run, shared_connection_data);

	pthread_attr_destroy(thread_settings);
}

void obs_ntr_connection_destroy()
{
	struct ntr_connection_data *temp_connection_data = shared_connection_data;
	shared_connection_data = NULL;

	if (temp_connection_data != NULL)
	{
		temp_connection_data->is_connected = false;
		pthread_join(temp_connection_data->net_thread, NULL);

		for (int screen_index = 0; screen_index < SCREEN_COUNT; screen_index++)
		{
			pthread_mutex_destroy(&temp_connection_data->buffer_mutex[screen_index]);
			bfree(temp_connection_data->uncompressed_buffer[screen_index]);
		}

		tjDestroy(temp_connection_data->decompressor_handle);

		closesocket(temp_connection_data->data_socket);

		for (int frame_index = 0; frame_index < CONCURRENT_FRAMES; frame_index++)
		{
			bfree(temp_connection_data->frames[frame_index].frame_data);
		}

		bfree(temp_connection_data);
	}
}


static const char *obs_ntr_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Ntr");
}

static void *obs_ntr_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	struct ntr_data *context = bzalloc(sizeof(struct ntr_data));
	context->source = source;

	if (obs_data_get_bool(settings, "owns_connection"))
	{
		context->pending_connect = true;
	}

	obs_source_update(source, settings);

	return context;
}

static void obs_ntr_destroy(void *data)
{
	struct ntr_data *context = data;

	if (context->owns_connection)
	{
		obs_ntr_connection_destroy();
	}

	if (context->texture != NULL)
	{
		obs_enter_graphics();
		gs_texture_destroy(context->texture);
		obs_leave_graphics();
	}

	bfree(context);
}

bool connect_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct ntr_data *context = data;

	context->pending_connect = true;
	obs_source_update(context->source, NULL);

	return true;
}

static obs_properties_t *obs_ntr_properties(void *data)
{
	struct ntr_data *context = data;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *screen_prop = obs_properties_add_list(props, "screen", obs_module_text("Ntr.Screen"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(screen_prop, obs_module_text("Ntr.Screen.Top"), SCREEN_TOP);
	obs_property_list_add_int(screen_prop, obs_module_text("Ntr.Screen.Bottom"), SCREEN_BOTTOM);

	if (context->owns_connection)
	{
		obs_properties_add_button(props, "connect", 
			(shared_connection_data == NULL ? obs_module_text("Ntr.Connect") : obs_module_text("Ntr.Disconnect")), 
			connect_clicked);
	}

	if (shared_connection_data == NULL)
	{
		obs_properties_add_bool(props, "owns_connection", obs_module_text("Ntr.OwnsConnection"));

		if (context->owns_connection)
		{
			obs_properties_add_text(props, "ip_address", obs_module_text("Ntr.IpAddress"), OBS_TEXT_DEFAULT);

			obs_properties_add_int_slider(props, "quality", obs_module_text("Ntr.Quality"), 10, 100, 1);

			obs_properties_add_int(props, "qos", obs_module_text("Ntr.Qos"), 0, 101, 1);

			obs_properties_add_int(props, "priority_factor", obs_module_text("Ntr.PriorityFactor"), 0, 10, 1);

			obs_property_t *priority_screen_prop = obs_properties_add_list(props, "priority_screen", obs_module_text("Ntr.PriorityScreen"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
			obs_property_list_add_int(priority_screen_prop, obs_module_text("Ntr.Screen.Top"), SCREEN_TOP);
			obs_property_list_add_int(priority_screen_prop, obs_module_text("Ntr.Screen.Bottom"), SCREEN_BOTTOM);
		}
	}

	return props;
}

static void obs_ntr_update(void *data, obs_data_t *settings)
{
	struct ntr_data *context = data;

	bool new_owns_connection = obs_data_get_bool(settings, "owns_connection");
	if (new_owns_connection != context->owns_connection)
	{
		context->owns_connection = new_owns_connection;
		obs_source_update_properties(context->source);
	}

	context->screen = obs_data_get_int(settings, "screen");

	dstr_copy(&context->connection_setup.ip_address, obs_data_get_string(settings, "ip_address"));
	context->connection_setup.quality = (int)obs_data_get_int(settings, "quality");
	context->connection_setup.qos = (int)obs_data_get_int(settings, "qos");
	context->connection_setup.priority_factor = (int)obs_data_get_int(settings, "priority_factor");
	context->connection_setup.priority_screen = (int)obs_data_get_int(settings, "priority_screen");

	obs_enter_graphics();

	if (context->texture == NULL)
	{
		gs_texture_destroy(context->texture);
	}

	context->texture = gs_texture_create(SCREEN_HEIGHT[context->screen], SCREEN_WIDTH[context->screen], GS_RGBA, 1, NULL, GS_DYNAMIC);

	obs_leave_graphics();

	if (context->pending_connect)
	{
		context->pending_connect = false;
		obs_source_update_properties(context->source);

		if (shared_connection_data != NULL)
		{
			obs_ntr_connection_destroy();
		}
		else if (!dstr_is_empty(&context->connection_setup.ip_address))
		{
			obs_ntr_connection_create(&context->connection_setup);
		}
	}
}

static void obs_ntr_tick(void *data, float seconds)
{
	struct ntr_data *context = data;

	if (shared_connection_data != NULL && shared_connection_data->last_frame_id[context->screen] != context->last_frame_id)
	{
		context->last_frame_id = shared_connection_data->last_frame_id[context->screen];

		pthread_mutex_lock(&shared_connection_data->buffer_mutex[context->screen]);
		obs_enter_graphics();
		gs_texture_set_image(context->texture, shared_connection_data->uncompressed_buffer[context->screen], SCREEN_HEIGHT[context->screen] * 4, false);
		obs_leave_graphics();
		pthread_mutex_unlock(&shared_connection_data->buffer_mutex[context->screen]);
	}
}

static void obs_ntr_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct ntr_data *context = data;

	if (context->texture != NULL)
	{
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"),
			context->texture);

		gs_matrix_push();
		gs_matrix_translate3f(0.0f, (float)SCREEN_HEIGHT[context->screen], 0.0f);
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(-90.0f));
		gs_draw_sprite(context->texture, 0,
			SCREEN_HEIGHT[context->screen], SCREEN_WIDTH[context->screen]);
		gs_matrix_pop();
	}

}

static uint32_t obs_ntr_getwidth(void *data)
{
	struct ntr_data *context = data;

	return SCREEN_WIDTH[context->screen];
}

static uint32_t obs_ntr_getheight(void *data)
{
	struct ntr_data *context = data;

	return SCREEN_HEIGHT[context->screen];
}

static void obs_ntr_defaults(obs_data_t *settings)
{

	obs_data_set_default_bool(settings, "owns_connection", false);

	obs_data_set_default_int(settings, "screen", SCREEN_TOP);

	obs_data_set_default_int(settings, "quality", 80);
	obs_data_set_default_int(settings, "priority_factor", 2);
	obs_data_set_default_int(settings, "qos", 100);
	obs_data_set_default_int(settings, "priority_screen", SCREEN_TOP);
}

struct obs_source_info obs_ntr_source = {
	.id             = "obs_ntr",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO,
	.create         = obs_ntr_create,
	.destroy        = obs_ntr_destroy,
	.update         = obs_ntr_update,
	.video_tick		= obs_ntr_tick,
	.get_name       = obs_ntr_get_name,
	.get_defaults   = obs_ntr_defaults,
	.get_width      = obs_ntr_getwidth,
	.get_height     = obs_ntr_getheight,
	.video_render   = obs_ntr_render,
	.get_properties = obs_ntr_properties
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-ntr", "en-US")

bool obs_module_load(void)
{
	obs_register_source(&obs_ntr_source);

	return true;
}

void obs_module_unload(void)
{
}
