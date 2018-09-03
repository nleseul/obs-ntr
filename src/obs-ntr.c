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
	NS_CMD_HEARTBEAT = 0,
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

#define TEMP_BUFFER_SIZE (320 * 400 * 4)

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
	bool net_thread_started;
	bool net_thread_exited;
	bool disconnect_requested;

	unsigned char *uncompressed_buffer[SCREEN_COUNT];
	int last_frame_id[SCREEN_COUNT];

	int dropped_frames;
	int total_processed_frames;
	float fps;
	uint64_t last_stat_time;
};

struct ntr_data
{
	obs_source_t *source;

	bool pending_connect;
	bool pending_property_refresh;

	enum ntr_screen screen;
	int last_frame_id;

	struct ntr_connection_setup connection_setup;

	pthread_t startup_remoteview_thread;
	bool startup_remoteview_thread_started;
	bool startup_remoteview_thread_running;

	gs_texture_t *texture;

	bool show_stats;
	bool mem_patch;
	obs_source_t *debug_text_source;
	uint64_t last_stat_time;
	bool update_debug_text;
};


void *obs_ntr_startup_remoteview_thread_run(void *data)
{
	struct ntr_data *context = data;

	context->startup_remoteview_thread_running = true;

	struct sockaddr_in client_address;
	client_address.sin_family = AF_INET;
	client_address.sin_addr.s_addr = htons(INADDR_ANY);
	client_address.sin_port = 0;

	struct sockaddr_in server_address;
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = inet_addr(context->connection_setup.ip_address.array);
	server_address.sin_port = htons(8000);


	SOCKET command_socket = socket(AF_INET, SOCK_STREAM, 0);

	if (command_socket == INVALID_SOCKET)
	{
		blog(LOG_WARNING, "obs-ntr: Failed to create socket to send startup command to NTR");
		goto exception;
	}

	if (bind(command_socket, (struct sockaddr *)&client_address, sizeof(struct sockaddr_in)) != 0)
	{
		blog(LOG_WARNING, "obs-ntr: Failed to bind socket to send startup command to NTR");
		goto exception;
	}

	if (connect(command_socket, (struct sockaddr *)&server_address, sizeof(struct sockaddr_in)) != 0)
	{
		blog(LOG_WARNING, "obs-ntr: Failed to connect to %s to send startup command to NTR", context->connection_setup.ip_address.array);
		goto exception;
	}

	struct ntr_command_packet start_command;
	start_command.magic_number = 0x12345678;
	start_command.sequence = 1;
	start_command.type = NS_TYPE_NORMAL;
	start_command.command = NS_CMD_REMOTEPLAY;
	start_command.args[0] = (context->connection_setup.priority_screen << 8) | context->connection_setup.priority_factor;
	start_command.args[1] = context->connection_setup.quality;
	start_command.args[2] = context->connection_setup.qos * 1024 * 1024 / 8;

	if (send(command_socket, (const char *)&start_command, sizeof(struct ntr_command_packet), 0) < 0)
	{
		blog(LOG_WARNING, "obs-ntr: Failed sending startup command to NTR");
		goto exception;
	}

	// The original NTRViewer sends three heartbeats after starting up remoteview for some reason. 
	// Probably this is used to verify that the NTR service on the 3DS didn't crash on boot or something?
	// In any case, NTR will not send any data packets back until we do this. 
	os_sleep_ms(100);

	struct ntr_command_packet heartbeat_command;
	heartbeat_command.magic_number = 0x12345678;
	heartbeat_command.type = NS_TYPE_NORMAL;
	heartbeat_command.command = NS_CMD_HEARTBEAT;
	for (int ping_index = 0; ping_index < 3; ping_index++)
	{
		heartbeat_command.sequence = ping_index + 2;
		if (send(command_socket, (const char *)&heartbeat_command, sizeof(struct ntr_command_packet), 0) < 0)
		{
			blog(LOG_WARNING, "obs-ntr: Failed sending initial heartbeat to NTR");
			goto exception;
		}

		os_sleep_ms(100);
	}
	if(context->mem_patch)
	{
		// This only works on firmwares 11.4 and later (tested on 11.6)
		char p114[84] = { 0x78, 0x56, 0x34, 0x12, 0xc0, 0x5d, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0a, 0x00, 
			0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x5b, 0x10, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
			0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
		char pbuf[] = { 0x70, 0x47 };
		
		if (send(command_socket, (const char *)&p114, 84, 0) < 0)
		{
			blog(LOG_WARNING, "obs-ntr: Failed sending memory patch initializer to NTR");
			goto exception;
		}
		if (send(command_socket, (const char *)&pbuf, 2, 0) < 0)
		{
			blog(LOG_WARNING, "obs-ntr: Failed sending memory patch payload to NTR");
			goto exception;
		}
	}
	closesocket(command_socket);

	blog(LOG_WARNING, "obs-ntr: Startup command sent successfully to NTR");

	context->startup_remoteview_thread_running = false;

	return 0;

exception:

	if (command_socket != INVALID_SOCKET)
	{
		closesocket(command_socket);
	}
	context->startup_remoteview_thread_running = false;
	return (void *)1;
}

#define DATA_SOCKET_TIMEOUT_DURATION_NS 1000000000

void *obs_ntr_net_thread_run(void *data)
{
	struct ntr_connection_data *connection_data = data;

	tjhandle decompressor_handle = tjInitDecompress();

	struct ntr_frame_data frames[CONCURRENT_FRAMES];
	for (int frame_index = 0; frame_index < CONCURRENT_FRAMES; frame_index++)
	{
		frames[frame_index].id = 0;
		frames[frame_index].packet_count = 0;
		frames[frame_index].expected_packet_count = 0;
		frames[frame_index].last_packet_data_size = 0;
		frames[frame_index].finished = 0;
		frames[frame_index].time_started = 0;

		frames[frame_index].frame_data = bzalloc(DATA_PACKET_DATA_SIZE * DATA_PACKET_MAX_COUNT);
	}

	connection_data->disconnect_requested = false;
	
	struct sockaddr_in data_socket_address_data;
	data_socket_address_data.sin_family = AF_INET;
	data_socket_address_data.sin_addr.s_addr = htonl(INADDR_ANY);
	data_socket_address_data.sin_port = htons(8001);

	SOCKET data_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (data_socket == INVALID_SOCKET)
	{
		blog(LOG_WARNING, "obs-ntr: Failed creating a data socket");
		goto exception;
	}

	if (bind(data_socket, (struct sockaddr *)&data_socket_address_data, sizeof(struct sockaddr_in)) != 0)
	{
		blog(LOG_WARNING, "obs-ntr: Failed binding a data socket");
		goto exception;
	}

	int buffer_size = 8 * 1024 * 1024;
	struct timeval timeout;
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;
	if (setsockopt(data_socket, SOL_SOCKET, SO_RCVBUF, (char *)&buffer_size, sizeof(int)) != 0)
	{
		blog(LOG_WARNING, "obs-ntr: Unable to set buffer size on data socket");
	}

	if (setsockopt(data_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(struct timeval)) != 0)
	{
		blog(LOG_WARNING, "obs-ntr: Unable to set timeout on data socket");
	}

	int frames_processed = 0;
	int frames_dumped = 0;
	uint64_t last_stat_time = obs_get_video_frame_time();
	uint64_t last_read_time = obs_get_video_frame_time();

	connection_data->last_stat_time = last_stat_time;

	while (!connection_data->disconnect_requested)
	{
		if (frames_processed >= 100)
		{
			uint64_t now = obs_get_video_frame_time();

			uint64_t elapsed_ms = (now - last_stat_time) / 1000000;
			float elapsed_seconds = (float)(elapsed_ms) / 1000.0f;
			float fps = (frames_processed - frames_dumped) / elapsed_seconds;

			connection_data->dropped_frames = frames_dumped;
			connection_data->total_processed_frames = frames_processed;
			connection_data->fps = fps;
			connection_data->last_stat_time = now;

			frames_processed = 0;
			frames_dumped = 0;
			last_stat_time = now;
		}

		struct sockaddr_in from_address;
		int from_address_length = sizeof(struct sockaddr_in);
		struct ntr_data_packet packet;
		int receive_result = recvfrom(data_socket, (char *)&packet, sizeof(struct ntr_data_packet), 0, (struct sockaddr *)&from_address, &from_address_length);

		if (receive_result > 0)
		{
			struct ntr_frame_data *active_frame = NULL;

			//blog(LOG_DEBUG, "obs-ntr: Received packet %d of frame id %d(%d)", packet.order, packet.id, packet.is_top);
			//blog(LOG_DEBUG, "obs-ntr: Current frames: %d(%d) %d(%d) %d(%d) %d(%d)", connection_data->frames[0].id, connection_data->frames[0].is_top,
				//connection_data->frames[1].id, connection_data->frames[1].is_top, connection_data->frames[2].id, connection_data->frames[2].is_top,
				//connection_data->frames[3].id, connection_data->frames[3].is_top);

			last_read_time = obs_get_video_frame_time();

			for (int frame_index = 0; frame_index < CONCURRENT_FRAMES; frame_index++)
			{
				if (frames[frame_index].id == packet.id && frames[frame_index].is_top == packet.is_top)
				{
					//blog(LOG_DEBUG, "obs-ntr: Found existing frame");
					active_frame = &frames[frame_index];
					break;
				}
			}

			if (active_frame == NULL)
			{
				int oldest_index = 0;
				uint64_t oldest_time = UINT64_MAX;
				for (int frame_index = 0; frame_index < CONCURRENT_FRAMES; frame_index++)
				{
					if (frames[frame_index].time_started < oldest_time)
					{
						oldest_time = frames[frame_index].time_started;
						oldest_index = frame_index;
					}
				}

				//blog(LOG_DEBUG, "obs-ntr: Replacing old frame %d", connection_data->frames[oldest_index].id);

				active_frame = &frames[oldest_index];
			}

			assert(active_frame != NULL);

			if (active_frame->id != packet.id || active_frame->is_top != packet.is_top || active_frame->finished)
			{
				if (!active_frame->finished)
				{
					//blog(LOG_DEBUG, "obs-ntr: Dumping frame %d (%d/%d) for frame %d (%d)", active_frame->id,
						//active_frame->packet_count, active_frame->expected_packet_count,
						//packet.id, packet.is_top);

					frames_processed++;
					frames_dumped++;
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

			//blog(LOG_DEBUG, "obs-ntr: Frame %d now has %d/%d packets", active_frame->id, active_frame->packet_count, active_frame->expected_packet_count);

			if (active_frame->expected_packet_count > 0 && active_frame->packet_count >= active_frame->expected_packet_count)
			{
				char local_decompress_buffer[TEMP_BUFFER_SIZE];

				//blog(LOG_DEBUG, "obs-ntr: Finishing frame %d with %d/%d packets", active_frame->id, active_frame->packet_count, active_frame->expected_packet_count);

				active_frame->finished = true;
				frames_processed++;

				int decompress_result = tjDecompress2(decompressor_handle, active_frame->frame_data,
					(active_frame->expected_packet_count - 1) * DATA_PACKET_DATA_SIZE + active_frame->last_packet_data_size,
					local_decompress_buffer, SCREEN_HEIGHT[packet.is_top], SCREEN_HEIGHT[packet.is_top] * 4,
					SCREEN_WIDTH[packet.is_top], TJPF_RGBA, 0);

				pthread_mutex_lock(&connection_data->buffer_mutex[packet.is_top]);
				memcpy(connection_data->uncompressed_buffer[packet.is_top], local_decompress_buffer, SCREEN_WIDTH[packet.is_top] * SCREEN_HEIGHT[packet.is_top] * 4);
				connection_data->last_frame_id[packet.is_top] = packet.id;
				pthread_mutex_unlock(&connection_data->buffer_mutex[packet.is_top]);
			}
		}
		else
		{
			uint64_t elapsed_ns_since_last_read = obs_get_video_frame_time() - last_read_time;

			if (elapsed_ns_since_last_read >= DATA_SOCKET_TIMEOUT_DURATION_NS)
			{
				blog(LOG_WARNING, "obs-ntr: Data socket received no data after %d ms; probably not active", elapsed_ns_since_last_read / 1000000);
				break;
			}
		}
		// It seems to be critical to our packet loss rate to wait for a non-zero duration here,
		// probably so the OS has adequate time to populate the socket's buffer. Note that I'm
		// passing 2, because the Windows implementation reduces the value by 1 for some reason. 
		os_sleep_ms(2);
	}

	for (int frame_index = 0; frame_index < CONCURRENT_FRAMES; frame_index++)
	{
		bfree(frames[frame_index].frame_data);
	}

	tjDestroy(decompressor_handle);

	closesocket(data_socket);
	connection_data->net_thread_exited = true;

	return 0;

exception:
	if (data_socket != INVALID_SOCKET)
	{
		closesocket(data_socket);
	}

	connection_data->net_thread_exited = true;
	return (void *)1;
}

static struct ntr_data *connection_owner = NULL;
static struct ntr_connection_data *shared_connection_data = NULL;

void obs_ntr_connection_create(struct ntr_data *owner_data)
{
	struct ntr_connection_data *temp_connection_data = bzalloc(sizeof(struct ntr_connection_data));

	for (int screen_index = 0; screen_index < SCREEN_COUNT; screen_index++)
	{
		temp_connection_data->uncompressed_buffer[screen_index] = bzalloc(SCREEN_WIDTH[screen_index] * SCREEN_HEIGHT[screen_index] * 4);

		pthread_mutex_init_value(&temp_connection_data->buffer_mutex[screen_index]);
		pthread_mutex_init(&temp_connection_data->buffer_mutex[screen_index], NULL);
	}

	shared_connection_data = temp_connection_data;

	shared_connection_data->net_thread_exited = false;
	shared_connection_data->net_thread_started = true;
	pthread_create(&shared_connection_data->net_thread, NULL, obs_ntr_net_thread_run, shared_connection_data);
}

void obs_ntr_connection_destroy()
{
	struct ntr_connection_data *temp_connection_data = shared_connection_data;
	shared_connection_data = NULL;

	if (temp_connection_data != NULL)
	{
		temp_connection_data->disconnect_requested = true;
		pthread_join(temp_connection_data->net_thread, NULL);

		for (int screen_index = 0; screen_index < SCREEN_COUNT; screen_index++)
		{
			pthread_mutex_destroy(&temp_connection_data->buffer_mutex[screen_index]);
			bfree(temp_connection_data->uncompressed_buffer[screen_index]);
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
		connection_owner = context;

		context->pending_connect = true;
	}

	obs_source_update(source, settings);

	return context;
}

static void obs_ntr_set_debug_text(struct ntr_data *context, const char *text)
{
	if (context->debug_text_source != NULL)
	{
		obs_data_t *new_text_data = obs_data_create();
		obs_data_set_string(new_text_data, "text", text);

		obs_source_update(context->debug_text_source, new_text_data);

		obs_data_release(new_text_data);
	}
}

static void obs_ntr_destroy(void *data)
{
	struct ntr_data *context = data;

	if (connection_owner == context && shared_connection_data != NULL)
	{
		obs_ntr_connection_destroy();
	}

	if (context->debug_text_source != NULL)
	{
		obs_source_release(context->debug_text_source);
	}

	if (context->texture != NULL)
	{
		obs_enter_graphics();
		gs_texture_destroy(context->texture);
		obs_leave_graphics();
	}

	bfree(context);
}

void obs_ntr_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct ntr_data *context = data;

	if (context->debug_text_source != NULL)
	{
		cb(context->source, context->debug_text_source, param);
	}
}

bool connect_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct ntr_data *context = data;

	context->pending_connect = true;
	obs_source_update(context->source, NULL);

	return true;
}


bool claim_connection_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct ntr_data *context = data;

	struct ntr_data *old_owner = connection_owner;
	connection_owner = context;

	context->pending_property_refresh = true;

	if (old_owner != NULL)
	{
		obs_data_t *old_owner_settings = obs_source_get_settings(old_owner->source);
		obs_data_set_bool(old_owner_settings, "owns_connection", false);
		obs_source_update(old_owner->source, old_owner_settings);
	}

	obs_data_t *new_owner_settings = obs_source_get_settings(context->source);
	obs_data_set_bool(new_owner_settings, "owns_connection", true);
	obs_source_update(context->source, new_owner_settings);

	return true;
}

bool start_remoteview_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct ntr_data *context = data;

	pthread_create(&context->startup_remoteview_thread, NULL, obs_ntr_startup_remoteview_thread_run, context);
	context->startup_remoteview_thread_started = true;

	context->pending_property_refresh = true;
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

	obs_properties_add_bool(props, "show_stats", obs_module_text("Ntr.ShowStats"));

	if (context == connection_owner)
	{
		obs_properties_add_bool(props, "mem_patch", obs_module_text("Ntr.MemPatch"));
		obs_properties_add_button(props, "connect", 
			(shared_connection_data == NULL ? obs_module_text("Ntr.Connect") : obs_module_text("Ntr.Disconnect")), 
			connect_clicked);

		obs_property_t *start_remoteview_prop = obs_properties_add_button(props, "start_remoteview", obs_module_text("Ntr.StartRemoteView"), start_remoteview_clicked);
		obs_property_set_enabled(start_remoteview_prop, shared_connection_data == NULL && !context->startup_remoteview_thread_started);

		obs_properties_add_text(props, "ip_address", obs_module_text("Ntr.IpAddress"), OBS_TEXT_DEFAULT);

		obs_properties_add_int_slider(props, "quality", obs_module_text("Ntr.Quality"), 10, 100, 1);

		obs_properties_add_int(props, "qos", obs_module_text("Ntr.Qos"), 0, 101, 1);

		obs_properties_add_int(props, "priority_factor", obs_module_text("Ntr.PriorityFactor"), 0, 10, 1);

		obs_property_t *priority_screen_prop = obs_properties_add_list(props, "priority_screen", obs_module_text("Ntr.PriorityScreen"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(priority_screen_prop, obs_module_text("Ntr.Screen.Top"), SCREEN_TOP);
		obs_property_list_add_int(priority_screen_prop, obs_module_text("Ntr.Screen.Bottom"), SCREEN_BOTTOM);
	}
	else
	{
		obs_property_t *claim_connection_prop = obs_properties_add_button(props, "claim_connection", obs_module_text("Ntr.ClaimConnection"), claim_connection_clicked);
		obs_property_set_enabled(claim_connection_prop, shared_connection_data == NULL);
	}

	return props;
}

static void obs_ntr_update(void *data, obs_data_t *settings)
{
	struct ntr_data *context = data;

	enum ntr_screen old_screen = context->screen;
	context->screen = obs_data_get_int(settings, "screen");

	dstr_copy(&context->connection_setup.ip_address, obs_data_get_string(settings, "ip_address"));
	context->connection_setup.quality = (int)obs_data_get_int(settings, "quality");
	context->connection_setup.qos = (int)obs_data_get_int(settings, "qos");
	context->connection_setup.priority_factor = (int)obs_data_get_int(settings, "priority_factor");
	context->connection_setup.priority_screen = (int)obs_data_get_int(settings, "priority_screen");

	context->show_stats = obs_data_get_bool(settings, "show_stats");
	context->mem_patch = obs_data_get_bool(settings, "mem_patch");

	if (context->show_stats && context->debug_text_source == NULL)
	{
		obs_data_t *text_defaults = obs_data_create();

		obs_data_t *text_font = obs_data_create();
		obs_data_set_int(text_font, "size", 24);
		obs_data_set_obj(text_defaults, "font", text_font);
		obs_data_release(text_font);
		text_font = NULL;

		context->debug_text_source = obs_source_create_private("text_gdiplus", NULL, text_defaults);

		obs_data_release(text_defaults);

		context->update_debug_text = true;
	}
	else if (!context->show_stats && context->debug_text_source != NULL)
	{
		obs_source_release(context->debug_text_source);
		context->debug_text_source = NULL;
	}

	if (context == connection_owner && !obs_data_get_bool(settings, "owns_connection"))
	{
		if (shared_connection_data != NULL)
		{
			obs_ntr_connection_destroy();
		}

		connection_owner = NULL;
	}

	if (old_screen != context->screen || context->texture == NULL)
	{
		obs_enter_graphics();

		if (context->texture != NULL)
		{
			gs_texture_destroy(context->texture);
		}

		context->texture = gs_texture_create(SCREEN_HEIGHT[context->screen], SCREEN_WIDTH[context->screen], GS_RGBA, 1, NULL, GS_DYNAMIC);

		obs_leave_graphics();
	}

	if (context->pending_property_refresh)
	{
		context->pending_property_refresh = false;

		obs_source_update_properties(context->source);
	}

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
			obs_ntr_connection_create(context);
		}

		context->update_debug_text = true;
	}

	if (context->debug_text_source != NULL && context->update_debug_text)
	{
		if (shared_connection_data == NULL)
		{
			obs_ntr_set_debug_text(context, obs_module_text("Ntr.ShowStats.NotConnected"));
		}
		else
		{
			struct dstr buffer;
			dstr_init_copy(&buffer, obs_module_text("Ntr.ShowStats.StatsDisplay"));

			char dropped_percent_buffer[8];
			char fps_buffer[8];

			float dropped_percent = 0.0f;
			if (shared_connection_data->total_processed_frames > 0)
			{
				dropped_percent = ((float)shared_connection_data->dropped_frames * 100.0f / shared_connection_data->total_processed_frames);
			}

			snprintf(dropped_percent_buffer, 8, "%.0f", dropped_percent);
			snprintf(fps_buffer, 8, "%.1f", shared_connection_data->fps);

			dstr_replace(&buffer, "%1", dropped_percent_buffer);
			dstr_replace(&buffer, "%2", fps_buffer);

			obs_ntr_set_debug_text(context, buffer.array);

			dstr_free(&buffer);
		}

		context->update_debug_text = false;
	}
}

static void obs_ntr_tick(void *data, float seconds)
{
	struct ntr_data *context = data;

	if (context->startup_remoteview_thread_started && !context->startup_remoteview_thread_running)
	{
		context->startup_remoteview_thread_started = false;

		context->pending_property_refresh = true;
		obs_source_update(context->source, NULL);
	}

	if (shared_connection_data != NULL)
	{
		if (shared_connection_data->last_frame_id[context->screen] != context->last_frame_id)
		{
			char local_image_buffer[TEMP_BUFFER_SIZE];

			pthread_mutex_lock(&shared_connection_data->buffer_mutex[context->screen]);
			context->last_frame_id = shared_connection_data->last_frame_id[context->screen];
			memcpy(local_image_buffer, shared_connection_data->uncompressed_buffer[context->screen], SCREEN_WIDTH[context->screen] * SCREEN_HEIGHT[context->screen] * 4);
			pthread_mutex_unlock(&shared_connection_data->buffer_mutex[context->screen]);

			obs_enter_graphics();
			gs_texture_set_image(context->texture, local_image_buffer, SCREEN_HEIGHT[context->screen] * 4, false);
			obs_leave_graphics();
		}

		if (context->debug_text_source != NULL && shared_connection_data->last_stat_time != context->last_stat_time)
		{
			context->last_stat_time = shared_connection_data->last_stat_time;
			context->update_debug_text = true;
			obs_source_update(context->source, NULL);
		}

		if (shared_connection_data->net_thread_started && shared_connection_data->net_thread_exited)
		{
			obs_ntr_connection_destroy();

			context->pending_property_refresh = true;
			context->update_debug_text = true;
			obs_source_update(context->source, NULL);
		}
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

	if (context->debug_text_source != NULL)
	{
		obs_source_video_render(context->debug_text_source);
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

	obs_data_set_default_bool(settings, "show_stats", false);
	obs_data_set_default_bool(settings, "mem_patch", false);
	
	obs_data_set_default_int(settings, "quality", 80);
	obs_data_set_default_int(settings, "priority_factor", 2);
	obs_data_set_default_int(settings, "qos", 100);
	obs_data_set_default_int(settings, "priority_screen", SCREEN_TOP);
}

struct obs_source_info obs_ntr_source = {
	.id                  = "obs_ntr",
	.type                = OBS_SOURCE_TYPE_INPUT,
	.output_flags        = OBS_SOURCE_VIDEO,
	.create              = obs_ntr_create,
	.destroy             = obs_ntr_destroy,
	.enum_active_sources = obs_ntr_enum_sources,
	.update              = obs_ntr_update,
	.video_tick          = obs_ntr_tick,
	.get_name            = obs_ntr_get_name,
	.get_defaults        = obs_ntr_defaults,
	.get_width           = obs_ntr_getwidth,
	.get_height          = obs_ntr_getheight,
	.video_render        = obs_ntr_render,
	.get_properties      = obs_ntr_properties
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
