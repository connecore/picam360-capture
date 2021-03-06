/**
 * picam360-driver @ picam360 project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <limits.h>
#include <sys/time.h>
#include <list>

#include "mjpeg_omx_decoder.h"

#define PLUGIN_NAME "mjpeg_omx_decoder"
#define DECODER_NAME "mjpeg_omx_decoder"

#include <rtp.h>

#define TIMEOUT_MS 2000

#ifdef __cplusplus
extern "C" {
#endif

#include "bcm_host.h"
#include "ilclient.h"
#include "mrevent.h"

#ifdef USE_GLES
#include "GLES2/gl2.h"
#include "EGL/egl.h"
#include "EGL/eglext.h"
#else
#include <GL/glew.h>
#include <GLFW/glfw3.h>
//#include "GL/gl.h"
//#include "GL/glut.h"
//#include "GL/glext.h"
#endif

#ifdef __cplusplus
}
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define OMX_INIT_STRUCTURE(a) \
    memset(&(a), 0, sizeof(a)); \
    (a).nSize = sizeof(a); \
    (a).nVersion.nVersion = OMX_VERSION; \
    (a).nVersion.s.nVersionMajor = OMX_VERSION_MAJOR; \
    (a).nVersion.s.nVersionMinor = OMX_VERSION_MINOR; \
    (a).nVersion.s.nRevision = OMX_VERSION_REVISION; \
    (a).nVersion.s.nStep = OMX_VERSION_STEP

#define MAX_CAM_NUM 2
#define TEXTURE_BUFFER_NUM 2

class _PACKET_T {
public:
	_PACKET_T() {
		len = 0;
		eof = false;
	}
	int len;
	char data[RTP_MAXPAYLOADSIZE];
	bool eof;
};
class _FRAME_T {
public:
	_FRAME_T() {
		pthread_mutex_init(&packets_mlock, NULL);
		mrevent_init(&packet_ready);
		xmp_info = false;
		memset(&quaternion, 0, sizeof(quaternion));
		memset(&offset, 0, sizeof(offset));
	}
	~_FRAME_T() {
		_FRAME_T *frame = this;
		while (!frame->packets.empty()) {
			_PACKET_T *packet;
			pthread_mutex_lock(&frame->packets_mlock);
			packet = *(frame->packets.begin());
			frame->packets.pop_front();
			if (frame->packets.empty()) {
				mrevent_reset(&frame->packet_ready);
			}
			pthread_mutex_unlock(&frame->packets_mlock);
			delete packet;
		}
	}
	std::list<_PACKET_T *> packets;
	pthread_mutex_t packets_mlock;
	MREVENT_T packet_ready;
	bool xmp_info;
	VECTOR4D_T quaternion;
	VECTOR4D_T offset;
};
class _SENDFRAME_ARG_T {
public:
	_SENDFRAME_ARG_T() {
		cam_run = false;
		cam_num = 0;
		framecount = 0;
		decodereqcount = 0;
		decodedcount = 0;
		fps = 0;
		frameskip = 0;
		pthread_mutex_init(&frames_mlock, NULL);
		mrevent_init(&frame_ready);
		mrevent_init(&buffer_ready);
		active_frame = NULL;
		last_frame = NULL;
		memset(egl_buffer, 0, sizeof(egl_buffer));
		egl_render = NULL;
		video_decode = NULL;
		resize = NULL;
		memset(tunnel, 0, sizeof(tunnel));
		fillbufferdone_count = 0;
		texture_width = 2048;
		texture_height = 2048;
		mrevent_init(&texture_update);
		n_buffers = 0;
	}
#ifdef USE_GLES
	EGLDisplay display;
	EGLContext context;
#endif
	bool cam_run;
	int cam_num;
	int framecount;
	int decodereqcount;
	int decodedcount;
	float fps;
	int frameskip;
	std::list<_FRAME_T *> frames;
	pthread_mutex_t frames_mlock;
	MREVENT_T frame_ready;
	MREVENT_T buffer_ready;
	pthread_t cam_thread;
	_FRAME_T *active_frame;
	_FRAME_T *last_frame;
	COMPONENT_T* video_decode;
	COMPONENT_T* resize;
	OMX_BUFFERHEADERTYPE* egl_buffer[2];
	COMPONENT_T* egl_render;
	TUNNEL_T tunnel[3];
	int fillbufferdone_count;
	int texture_width;
	int texture_height;
	MREVENT_T texture_update;
	GLuint *cam_texture; //double buffer
	void *egl_images[TEXTURE_BUFFER_NUM]; //double buffer
	int n_buffers;
};

static PLUGIN_HOST_T *lg_plugin_host = NULL;
static _SENDFRAME_ARG_T *lg_send_frame_arg[MAX_CAM_NUM] = { };

typedef struct _mjpeg_omx_decoder {
	DECODER_T super;

	int cam_num;
	_SENDFRAME_ARG_T *send_frame_arg;
} mjpeg_omx_decoder;

static void my_fill_buffer_done(void* data, COMPONENT_T* comp) {
	_SENDFRAME_ARG_T *send_frame_arg = (_SENDFRAME_ARG_T*) data;

	//printf("buffer done \n");
	int cam_num = send_frame_arg->cam_num;
	int cur = send_frame_arg->fillbufferdone_count % send_frame_arg->n_buffers;
	if (lg_plugin_host) {
		lg_plugin_host->lock_texture();
		lg_plugin_host->set_cam_texture_cur(cam_num, cur);
		if (send_frame_arg->last_frame && send_frame_arg->last_frame->xmp_info) {
			lg_plugin_host->set_camera_quaternion(cam_num, send_frame_arg->last_frame->quaternion);
			lg_plugin_host->set_camera_offset(cam_num, send_frame_arg->last_frame->offset);
		}
		lg_plugin_host->unlock_texture();
		lg_plugin_host->send_event(PICAM360_HOST_NODE_ID, PICAM360_CAPTURE_EVENT_TEXTURE0_UPDATED + cam_num);
	}
	send_frame_arg->fillbufferdone_count++;
	cur = send_frame_arg->fillbufferdone_count % send_frame_arg->n_buffers;
	if (OMX_FillThisBuffer(ilclient_get_handle(send_frame_arg->egl_render), send_frame_arg->egl_buffer[cur]) != OMX_ErrorNone) {
		printf("OMX_FillThisBuffer failed in callback\n");
		//exit(1);
	}
}
static int port_setting_changed_again(_SENDFRAME_ARG_T *send_frame_arg) {
	ilclient_disable_port(send_frame_arg->video_decode, 131);
	ilclient_disable_port(send_frame_arg->resize, 60);

	OMX_ERRORTYPE omx_err = OMX_ErrorNone;

	OMX_PARAM_PORTDEFINITIONTYPE portdef;

	// need to setup the input for the resizer with the output of the
	// decoder
	portdef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
	portdef.nVersion.nVersion = OMX_VERSION;
	portdef.nPortIndex = 131;
	OMX_GetParameter(ILC_GET_HANDLE(send_frame_arg->video_decode), OMX_IndexParamPortDefinition, &portdef);

	uint32_t image_width = (unsigned int) portdef.format.image.nFrameWidth;
	uint32_t image_height = (unsigned int) portdef.format.image.nFrameHeight;
	uint32_t image_inner_width = MIN(image_width, image_height);

	// tell resizer input what the decoder output will be providing
	portdef.nPortIndex = 60;
	OMX_SetParameter(ILC_GET_HANDLE(send_frame_arg->resize), OMX_IndexParamPortDefinition, &portdef);

	OMX_CONFIG_RECTTYPE omx_crop_req;
	OMX_INIT_STRUCTURE(omx_crop_req);
	omx_crop_req.nPortIndex = 60;
	omx_crop_req.nLeft = (image_width - image_inner_width) / 2;
	omx_crop_req.nWidth = image_inner_width;
	omx_crop_req.nTop = (image_height - image_inner_width) / 2;
	omx_crop_req.nHeight = image_inner_width;
	OMX_SetConfig(ILC_GET_HANDLE(send_frame_arg->resize), OMX_IndexConfigCommonInputCrop, &omx_crop_req);
	//printf("crop %d, %d, %d, %d\n", omx_crop_req.nLeft, omx_crop_req.nTop,
	//		omx_crop_req.nWidth, omx_crop_req.nHeight);

	// enable output of decoder and input of resizer (ie enable tunnel)
	ilclient_enable_port(send_frame_arg->video_decode, 131);
	ilclient_enable_port(send_frame_arg->resize, 60);

	// need to wait for this event
	ilclient_wait_for_event(send_frame_arg->video_decode, OMX_EventPortSettingsChanged, 131, 1, 0, 0, 0, TIMEOUT_MS);
}

static int port_setting_changed(_SENDFRAME_ARG_T *send_frame_arg) {
	OMX_ERRORTYPE omx_err = OMX_ErrorNone;

	OMX_PARAM_PORTDEFINITIONTYPE portdef;

	// need to setup the input for the resizer with the output of the
	// decoder
	portdef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
	portdef.nVersion.nVersion = OMX_VERSION;
	portdef.nPortIndex = 131;
	OMX_GetParameter(ILC_GET_HANDLE(send_frame_arg->video_decode), OMX_IndexParamPortDefinition, &portdef);

	uint32_t image_width = (unsigned int) portdef.format.image.nFrameWidth;
	uint32_t image_height = (unsigned int) portdef.format.image.nFrameHeight;
	uint32_t image_inner_width = MIN(image_width, image_height);

	send_frame_arg->texture_width = image_inner_width;
	send_frame_arg->texture_height = image_inner_width;
	if (lg_plugin_host) {
		lg_plugin_host->set_texture_size(send_frame_arg->texture_width, send_frame_arg->texture_height);
	}
	{
		char cmd[256];
		sprintf(cmd, PLUGIN_NAME ".set_texture_size %d=%d,%d", send_frame_arg->cam_num, send_frame_arg->texture_width, send_frame_arg->texture_height);
		mrevent_reset(&send_frame_arg->texture_update);
		lg_plugin_host->send_command(cmd);
		mrevent_wait(&send_frame_arg->texture_update, 0);
	}

	// tell resizer input what the decoder output will be providing
	portdef.nPortIndex = 60;
	OMX_SetParameter(ILC_GET_HANDLE(send_frame_arg->resize), OMX_IndexParamPortDefinition, &portdef);

	if (ilclient_setup_tunnel(send_frame_arg->tunnel, 0, 0) != 0) {
		printf("fail tunnel 0\n");
		return -7;
	}

	// put resizer in idle state (this allows the outport of the decoder
	// to become enabled)
	ilclient_change_component_state(send_frame_arg->resize, OMX_StateIdle);

	OMX_CONFIG_RECTTYPE omx_crop_req;
	OMX_INIT_STRUCTURE(omx_crop_req);
	omx_crop_req.nPortIndex = 60;
	omx_crop_req.nLeft = (image_width - image_inner_width) / 2;
	omx_crop_req.nWidth = image_inner_width;
	omx_crop_req.nTop = (image_height - image_inner_width) / 2;
	omx_crop_req.nHeight = image_inner_width;
	OMX_SetConfig(ILC_GET_HANDLE(send_frame_arg->resize), OMX_IndexConfigCommonInputCrop, &omx_crop_req);
	//printf("crop %d, %d, %d, %d\n", omx_crop_req.nLeft, omx_crop_req.nTop,
	//		omx_crop_req.nWidth, omx_crop_req.nHeight);

	// query output buffer requirements for resizer
	portdef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
	portdef.nVersion.nVersion = OMX_VERSION;
	portdef.nPortIndex = 61;
	OMX_GetParameter(ILC_GET_HANDLE(send_frame_arg->resize), OMX_IndexParamPortDefinition, &portdef);

	// change output color format and dimensions to match input
	portdef.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
	portdef.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
	portdef.format.image.nFrameWidth = send_frame_arg->texture_width;
	portdef.format.image.nFrameHeight = send_frame_arg->texture_height;
	portdef.format.image.nStride = 0;
	portdef.format.image.nSliceHeight = 16;
	portdef.format.image.bFlagErrorConcealment = OMX_FALSE;

	OMX_SetParameter(ILC_GET_HANDLE(send_frame_arg->resize), OMX_IndexParamPortDefinition, &portdef);

	// grab output requirements again to get actual buffer size
	// requirement (and buffer count requirement!)
	OMX_GetParameter(ILC_GET_HANDLE(send_frame_arg->resize), OMX_IndexParamPortDefinition, &portdef);

	// move resizer into executing state
	ilclient_change_component_state(send_frame_arg->resize, OMX_StateExecuting);

	// show some logging so user knows it's working
	printf("Width: %u Height: %u Output Color Format: 0x%x Buffer Size: %u\n", (unsigned int) portdef.format.image.nFrameWidth, (unsigned int) portdef.format.image.nFrameHeight,
			(unsigned int) portdef.format.image.eColorFormat, (unsigned int) portdef.nBufferSize);
	fflush (stdout);

	//resize -> egl_render

	//setup tunnel
	if (ilclient_setup_tunnel(send_frame_arg->tunnel + 1, 0, 0) != 0) {
		printf("fail tunnel 1\n");
		return -7;
	}

	// Set lg_egl_render to idle
	ilclient_change_component_state(send_frame_arg->egl_render, OMX_StateIdle);

	// Obtain the information about the output port.
	OMX_PARAM_PORTDEFINITIONTYPE port_format;
	OMX_INIT_STRUCTURE(port_format);
	port_format.nPortIndex = 221;
	omx_err = OMX_GetParameter(ILC_GET_HANDLE(send_frame_arg->egl_render), OMX_IndexParamPortDefinition, &port_format);
	if (omx_err != OMX_ErrorNone) {
		printf("%s - OMX_GetParameter OMX_IndexParamPortDefinition omx_err(0x%08x)", __func__, omx_err);
		exit(1);
	}

	port_format.nBufferCountActual = send_frame_arg->n_buffers;
	omx_err = OMX_SetParameter(ILC_GET_HANDLE(send_frame_arg->egl_render), OMX_IndexParamPortDefinition, &port_format);
	if (omx_err != OMX_ErrorNone) {
		printf("%s - OMX_SetParameter OMX_IndexParamPortDefinition omx_err(0x%08x)", __func__, omx_err);
		exit(1);
	}

	// Enable the output port and tell lg_egl_render to use the texture as a buffer
	//ilclient_enable_port(lg_egl_render, 221); THIS BLOCKS SO CAN'T BE USED
	if (OMX_SendCommand(ILC_GET_HANDLE(send_frame_arg->egl_render), OMX_CommandPortEnable, 221, NULL) != OMX_ErrorNone) {
		printf("OMX_CommandPortEnable failed.\n");
		exit(1);
	}

	for (int i = 0; i < send_frame_arg->n_buffers; i++) {
		OMX_STATETYPE state;
		OMX_GetState(ILC_GET_HANDLE(send_frame_arg->egl_render), &state);
		if (state != OMX_StateIdle) {
			if (state != OMX_StateLoaded) {
				ilclient_change_component_state(send_frame_arg->egl_render, OMX_StateLoaded);
			}
			ilclient_change_component_state(send_frame_arg->egl_render, OMX_StateIdle);
		}
		omx_err = OMX_UseEGLImage(ILC_GET_HANDLE(send_frame_arg->egl_render), &send_frame_arg->egl_buffer[i], 221, (void*) i, send_frame_arg->egl_images[i]);
		if (omx_err != OMX_ErrorNone) {
			printf("OMX_UseEGLImage failed. 0x%x\n", omx_err);
			exit(1);
		}
	}

	// Set lg_egl_render to executing
	ilclient_change_component_state(send_frame_arg->egl_render, OMX_StateExecuting);

	// Request lg_egl_render to write data to the texture buffer
	if (OMX_FillThisBuffer(ILC_GET_HANDLE(send_frame_arg->egl_render), send_frame_arg->egl_buffer[0]) != OMX_ErrorNone) {
		printf("OMX_FillThisBuffer failed.\n");
		exit(1);
	}

	printf("start fill buffer\n");
	return 0;
}

static void *sendframe_thread_func(void* arg) {
	pthread_setname_np(pthread_self(), "MJPEG SENDFRAME");

	_SENDFRAME_ARG_T *send_frame_arg = (_SENDFRAME_ARG_T*) arg;
	int last_framecount = send_frame_arg->framecount;
	struct timeval last_time = { };
	gettimeofday(&last_time, NULL);

	int cam_num = send_frame_arg->cam_num;

	OMX_VIDEO_PARAM_PORTFORMATTYPE format;
	COMPONENT_T *list[4];
	ILCLIENT_T *client;
	int status = 0;
	unsigned int data_len = 0;

	memset(list, 0, sizeof(list));

	if ((client = ilclient_init()) == NULL) {
		return (void *) -3;
	}

	if (OMX_Init() != OMX_ErrorNone) {
		ilclient_destroy(client);
		return (void *) -4;
	}

	// callback
	ilclient_set_fill_buffer_done_callback(client, my_fill_buffer_done, (void*) send_frame_arg);

	// create video_decode
	if (ilclient_create_component(client, &send_frame_arg->video_decode, (char*) "video_decode", (ILCLIENT_CREATE_FLAGS_T)(ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_INPUT_BUFFERS)) != 0)
		status = -14;
	list[0] = send_frame_arg->video_decode;

	// create resize
	if (status == 0 && ilclient_create_component(client, &send_frame_arg->resize, (char*) "resize", (ILCLIENT_CREATE_FLAGS_T)(ILCLIENT_DISABLE_ALL_PORTS)) != 0)
		status = -14;
	list[1] = send_frame_arg->resize;

	// create lg_egl_render
	if (status == 0
			&& ilclient_create_component(client, &send_frame_arg->egl_render, (char*) "egl_render", (ILCLIENT_CREATE_FLAGS_T)(ILCLIENT_DISABLE_ALL_PORTS | ILCLIENT_ENABLE_OUTPUT_BUFFERS)) != 0)
		status = -14;
	list[2] = send_frame_arg->egl_render;

	set_tunnel(send_frame_arg->tunnel, send_frame_arg->video_decode, 131, send_frame_arg->resize, 60);
	set_tunnel(send_frame_arg->tunnel + 1, send_frame_arg->resize, 61, send_frame_arg->egl_render, 220);

	if (status == 0)
		ilclient_change_component_state(send_frame_arg->video_decode, OMX_StateIdle);

	memset(&format, 0, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
	format.nSize = sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE);
	format.nVersion.nVersion = OMX_VERSION;
	format.nPortIndex = 130;
	format.eCompressionFormat = OMX_VIDEO_CodingMJPEG;

	if (status == 0 && OMX_SetParameter(ILC_GET_HANDLE(send_frame_arg->video_decode), OMX_IndexParamVideoPortFormat, &format) == OMX_ErrorNone
			&& ilclient_enable_port_buffers(send_frame_arg->video_decode, 130, NULL, NULL, NULL) == 0) {
		OMX_BUFFERHEADERTYPE *buf;
		int port_settings_changed = 0;
		int first_packet = 1;

		ilclient_change_component_state(send_frame_arg->video_decode, OMX_StateExecuting);

		printf("milestone\n");

		while (send_frame_arg->cam_run) {
			int res = mrevent_wait(&send_frame_arg->frame_ready, 100 * 1000);
			if (res != 0) {
				continue;
			}
			_FRAME_T *frame = NULL;
			pthread_mutex_lock(&send_frame_arg->frames_mlock);
			while (1) {
				if (!send_frame_arg->frames.empty()) {
					frame = *(send_frame_arg->frames.begin());
					send_frame_arg->frames.pop_front();
					send_frame_arg->framecount++;
				}
				if (send_frame_arg->frames.empty()) {
					mrevent_reset(&send_frame_arg->frame_ready);
					break;
				} else {
					send_frame_arg->frameskip++;
					delete frame; //skip frame
					frame = NULL;
				}
			}
			pthread_mutex_unlock(&send_frame_arg->frames_mlock);
			if (frame == NULL) {
				continue;
			}
			while (send_frame_arg->cam_run) {
				{ //fps
					struct timeval time = { };
					gettimeofday(&time, NULL);

					struct timeval diff;
					timersub(&time, &last_time, &diff);
					float diff_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;
					if (diff_sec > 1.0) {
						float tmp = (float) (send_frame_arg->framecount - last_framecount) / diff_sec;
						float w = diff_sec / 10;
						send_frame_arg->fps = send_frame_arg->fps * (1.0 - w) + tmp * w;

						last_framecount = send_frame_arg->framecount;
						last_time = time;
					}
				}
				int res = mrevent_wait(&frame->packet_ready, 100 * 1000);
				if (res != 0) {
					continue;
				}
				_PACKET_T *packet = NULL;
				pthread_mutex_lock(&frame->packets_mlock);
				if (!frame->packets.empty()) {
					packet = *(frame->packets.begin());
					frame->packets.pop_front();
				}
				if (frame->packets.empty()) {
					mrevent_reset(&frame->packet_ready);
				}
				pthread_mutex_unlock(&frame->packets_mlock);
				if (packet == NULL) {
					fprintf(stderr, "packet is null\n");
					continue;
				}
				// send the packet

				buf = ilclient_get_input_buffer(send_frame_arg->video_decode, 130, 1);

				data_len = MIN((int )buf->nAllocLen, packet->len);
				memcpy(buf->pBuffer, packet->data, data_len);

				if (ilclient_remove_event(send_frame_arg->video_decode, OMX_EventPortSettingsChanged, 131, 0, 0, 1) == 0) {
					printf("port changed %d\n", cam_num);

					if (port_settings_changed) {
						status = port_setting_changed_again(send_frame_arg);
					} else {
						status = port_setting_changed(send_frame_arg);
						port_settings_changed = 1;
					}

					if (status != 0) {
						status = -7;
						break;
					}
				}
				if (!data_len) {
					break;
				}
				buf->nFilledLen = data_len;

				buf->nOffset = 0;
				if (first_packet) {
					buf->nFlags = OMX_BUFFERFLAG_STARTTIME;
					first_packet = 0;
				} else {
					buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;
				}

				if (packet->eof) {
					buf->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;
				}

				if (OMX_EmptyThisBuffer(ILC_GET_HANDLE(send_frame_arg->video_decode), buf) != OMX_ErrorNone) {
					status = -6;
					break;
				}

				if (packet->eof) {
					if (send_frame_arg->last_frame != NULL) {
						delete send_frame_arg->last_frame;
					}
					send_frame_arg->last_frame = frame;
					delete packet;
					break;
				} else {
					delete packet;
				}
			}
		}

		buf->nFilledLen = 0;
		buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN | OMX_BUFFERFLAG_EOS;

		if (OMX_EmptyThisBuffer(ILC_GET_HANDLE(send_frame_arg->video_decode), buf) != OMX_ErrorNone)
			status = -20;

		// need to flush the renderer to allow video_decode to disable its input port
		ilclient_flush_tunnels(send_frame_arg->tunnel, 0);

		ilclient_disable_port_buffers(send_frame_arg->video_decode, 130, NULL, NULL, NULL);
	}

	ilclient_disable_tunnel(send_frame_arg->tunnel);
	ilclient_teardown_tunnels(send_frame_arg->tunnel);

	ilclient_state_transition(list, OMX_StateIdle);
	ilclient_state_transition(list, OMX_StateLoaded);

	ilclient_cleanup_components(list);

	OMX_Deinit();

	ilclient_destroy(client);
	return (void *) status;
}

static void parse_xml(char *xml, _FRAME_T *frame) {
	frame->xmp_info = true;

	char *q_str = NULL;
	q_str = strstr(xml, "<quaternion");
	if (q_str) {
		sscanf(q_str, "<quaternion x=\"%f\" y=\"%f\" z=\"%f\" w=\"%f\" />", &frame->quaternion.x, &frame->quaternion.y, &frame->quaternion.z, &frame->quaternion.w);
	}
	q_str = strstr(xml, "<offset");
	if (q_str) {
		sscanf(q_str, "<offset x=\"%f\" y=\"%f\" yaw=\"%f\" horizon_r=\"%f\" />", &frame->offset.x, &frame->offset.y, &frame->offset.z, &frame->offset.w);
	}
}

static void decode(void *obj, unsigned char *data, int data_len) {
	mjpeg_omx_decoder *_this = (mjpeg_omx_decoder*) obj;
	if (!lg_send_frame_arg[_this->cam_num]) {
		return;
	}
	_SENDFRAME_ARG_T *send_frame_arg = lg_send_frame_arg[_this->cam_num];
	if (send_frame_arg->active_frame == NULL) {
		if (data[0] == 0xFF && data[1] == 0xD8) { //SOI
			send_frame_arg->active_frame = new _FRAME_T;

			pthread_mutex_lock(&send_frame_arg->frames_mlock);
			send_frame_arg->frames.push_back(send_frame_arg->active_frame);
			pthread_mutex_unlock(&send_frame_arg->frames_mlock);
			mrevent_trigger(&send_frame_arg->frame_ready);

			if (data[2] == 0xFF && data[3] == 0xE1) { //xmp
				int xmp_len = 0;
				xmp_len = ((unsigned char*) data)[4] << 8;
				xmp_len += ((unsigned char*) data)[5];

				char *xml = (char*) data + strlen((char*) data) + 1;
				parse_xml(xml, send_frame_arg->active_frame);
			}
		}
	}
	if (send_frame_arg->active_frame != NULL) {
		_PACKET_T *packet = new _PACKET_T;
		{
			packet->len = data_len;
			if (packet->len > RTP_MAXPAYLOADSIZE) {
				fprintf(stderr, "packet length exceeded. %d\n", packet->len);
				packet->len = RTP_MAXPAYLOADSIZE;
			}
			memcpy(packet->data, data, packet->len);
		}
		if (data[data_len - 2] == 0xFF && data[data_len - 1] == 0xD9) { //EOI
			packet->eof = true;

			pthread_mutex_lock(&send_frame_arg->active_frame->packets_mlock);
			send_frame_arg->active_frame->packets.push_back(packet);
			mrevent_trigger(&send_frame_arg->active_frame->packet_ready);
			pthread_mutex_unlock(&send_frame_arg->active_frame->packets_mlock);

			send_frame_arg->active_frame = NULL;
		} else {
			pthread_mutex_lock(&send_frame_arg->active_frame->packets_mlock);
			send_frame_arg->active_frame->packets.push_back(packet);
			pthread_mutex_unlock(&send_frame_arg->active_frame->packets_mlock);
			mrevent_trigger(&send_frame_arg->active_frame->packet_ready);
		}
	}

}
static void init(void *obj, int cam_num, void *display, void *context, void *cam_texture, int n_buffers) {
	mjpeg_omx_decoder *_this = (mjpeg_omx_decoder*) obj;

	_this->cam_num = MAX(MIN(cam_num,MAX_CAM_NUM-1), 0);
	if (lg_send_frame_arg[_this->cam_num]) {
		return;
	}
	lg_send_frame_arg[_this->cam_num] = new _SENDFRAME_ARG_T;
#ifdef USE_GLES
	lg_send_frame_arg[_this->cam_num]->display = display;
	lg_send_frame_arg[_this->cam_num]->context = context;
#endif
	lg_send_frame_arg[_this->cam_num]->cam_num = _this->cam_num;
	lg_send_frame_arg[_this->cam_num]->cam_texture = (GLuint*)cam_texture;
	lg_send_frame_arg[_this->cam_num]->n_buffers = n_buffers;

	lg_send_frame_arg[_this->cam_num]->cam_run = true;
	pthread_create(&lg_send_frame_arg[_this->cam_num]->cam_thread, NULL, sendframe_thread_func, (void*) lg_send_frame_arg[_this->cam_num]);
}
static void release(void *obj) {
	mjpeg_omx_decoder *_this = (mjpeg_omx_decoder*) obj;

	if (!lg_send_frame_arg[_this->cam_num]) {
		return;
	}

	lg_send_frame_arg[_this->cam_num]->cam_run = false;
	pthread_join(lg_send_frame_arg[_this->cam_num]->cam_thread, NULL);

	delete lg_send_frame_arg[_this->cam_num];
	lg_send_frame_arg[_this->cam_num] = NULL;
	free(obj);
}

static float get_fps(void *obj) {
	mjpeg_omx_decoder *_this = (mjpeg_omx_decoder*) obj;

	if (!lg_send_frame_arg[_this->cam_num]) {
		return 0;
	}
	return lg_send_frame_arg[_this->cam_num]->fps;
}
static int get_frameskip(void *obj) {
	mjpeg_omx_decoder *_this = (mjpeg_omx_decoder*) obj;

	if (!lg_send_frame_arg[_this->cam_num]) {
		return 0;
	}
	return lg_send_frame_arg[_this->cam_num]->frameskip;
}

static void switch_buffer(void *obj) {
	mjpeg_omx_decoder *_this = (mjpeg_omx_decoder*) obj;

	if (!lg_send_frame_arg[_this->cam_num]) {
		return;
	}
	int res = mrevent_wait(&lg_send_frame_arg[_this->cam_num]->buffer_ready, 1000 * 1000);
	if (res != 0) {
		mrevent_trigger(&lg_send_frame_arg[_this->cam_num]->buffer_ready);
	}
}

static void create_decoder(void *user_data, DECODER_T **out_decoder) {
	DECODER_T *decoder = (DECODER_T*) malloc(sizeof(mjpeg_omx_decoder));
	memset(decoder, 0, sizeof(mjpeg_omx_decoder));
	strcpy(decoder->name, DECODER_NAME);
	decoder->release = release;
	decoder->init = init;
	decoder->get_fps = get_fps;
	decoder->get_frameskip = get_frameskip;
	decoder->decode = decode;
	decoder->switch_buffer = switch_buffer;
	decoder->release = release;
	decoder->user_data = decoder;

	if (out_decoder) {
		*out_decoder = decoder;
	}
}

static int command_handler(void *user_data, const char *_buff) {
	int opt;
	int ret = 0;
	char buff[256];
	strncpy(buff, _buff, sizeof(buff));
	char *cmd = strtok(buff, " \n");
	if (cmd == NULL) {
		//do nothing
	} else if (strncmp(cmd, PLUGIN_NAME ".set_texture_size", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			int i;
			int width;
			int height;
			sscanf(param, "%d=%d,%d", &i, &width, &height);
			for (int j = 0; j < lg_send_frame_arg[i]->n_buffers; j++) {
				glGenTextures(1, &lg_send_frame_arg[i]->cam_texture[j]);

				glBindTexture(GL_TEXTURE_2D, lg_send_frame_arg[i]->cam_texture[j]);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, lg_send_frame_arg[i]->texture_width, lg_send_frame_arg[i]->texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifdef USE_GLES
#ifdef BCM_HOST
				/* Create EGL Image */
				lg_send_frame_arg[i]->egl_images[j] = eglCreateImageKHR(lg_send_frame_arg[i]->display, lg_send_frame_arg[i]->context, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer) lg_send_frame_arg[i]->cam_texture[j], 0);

				if (lg_send_frame_arg[i]->egl_images[j] == EGL_NO_IMAGE_KHR) {
					printf("eglCreateImageKHR failed.\n");
					exit(1);
				}
#endif
#endif
			}
			mrevent_trigger(&lg_send_frame_arg[i]->texture_update);
			printf("set_texture_size : completed\n");
		}
	}
}

static void event_handler(void *user_data, uint32_t node_id, uint32_t event_id) {
}

static void init_options(void *user_data, json_t *options) {
}

static void save_options(void *user_data, json_t *options) {
}

void create_plugin(PLUGIN_HOST_T *plugin_host, PLUGIN_T **_plugin) {
	lg_plugin_host = plugin_host;

	{
		PLUGIN_T *plugin = (PLUGIN_T*) malloc(sizeof(PLUGIN_T));
		memset(plugin, 0, sizeof(PLUGIN_T));
		strcpy(plugin->name, PLUGIN_NAME);
		plugin->release = release;
		plugin->command_handler = command_handler;
		plugin->event_handler = event_handler;
		plugin->init_options = init_options;
		plugin->save_options = save_options;
		plugin->get_info = NULL;
		plugin->user_data = plugin;

		*_plugin = plugin;
	}
	{
		DECODER_FACTORY_T *decoder_factory = (DECODER_FACTORY_T*) malloc(sizeof(DECODER_FACTORY_T));
		memset(decoder_factory, 0, sizeof(DECODER_FACTORY_T));
		strcpy(decoder_factory->name, DECODER_NAME);
		decoder_factory->release = release;
		decoder_factory->create_decoder = create_decoder;

		lg_plugin_host->add_decoder_factory(decoder_factory);
	}
}
