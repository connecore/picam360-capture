#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <fcntl.h>
#include <pthread.h>
#include <editline/readline.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>

#ifdef BCM_HOST
#include "bcm_host.h"
#endif

#include "picam360_capture.h"
#include "gl_program.h"
#include "auto_calibration.h"
#include "manual_mpu.h"
#include "img/logo_png.h"

#include <mat4/type.h>
//#include <mat4/create.h>
#include <mat4/identity.h>
#include <mat4/rotateX.h>
#include <mat4/rotateY.h>
#include <mat4/rotateZ.h>
//#include <mat4/scale.h>
#include <mat4/multiply.h>
#include <mat4/transpose.h>
#include <mat4/fromQuat.h>
//#include <mat4/perspective.h>
#include <mat4/invert.h>

//json parser
#include <jansson.h>

#include <opencv/highgui.h>

#ifdef _WIN64
//define something for Windows (64-bit)
#elif _WIN32
//define something for Windows (32-bit)
#elif __APPLE__
#include "TargetConditionals.h"
#if TARGET_OS_IPHONE && TARGET_IPHONE_SIMULATOR
// define something for simulator
#elif TARGET_OS_IPHONE
// define something for iphone
#else
#define TARGET_OS_OSX 1
// define something for OSX
#endif
#elif __linux
// linux
#include <editline/history.h>
#elif __unix // all unices not caught above
// Unix
#elif __posix
// POSIX
#endif

#define PATH "./"
#define PICAM360_HISTORY_FILE ".picam360_history"
#define PLUGIN_NAME "picam360"

#ifndef M_PI
#define M_PI 3.141592654
#endif

static void init_ogl(PICAM360CAPTURE_T *state);
static void init_model_proj(PICAM360CAPTURE_T *state);
static void init_textures(PICAM360CAPTURE_T *state);
static void init_options(PICAM360CAPTURE_T *state);
static void save_options(PICAM360CAPTURE_T *state);
static void init_options_ex(PICAM360CAPTURE_T *state);
static void save_options_ex(PICAM360CAPTURE_T *state);
static void exit_func(void);
static void redraw_render_texture(PICAM360CAPTURE_T *state, FRAME_T *frame, RENDERER_T *renderer, VECTOR4D_T view_quat);
static void redraw_scene(PICAM360CAPTURE_T *state, FRAME_T *frame, MODEL_T *model);
static void redraw_info(PICAM360CAPTURE_T *state, FRAME_T *frame);
static void get_info_str(char *buff, int buff_len);
static void get_menu_str(char *buff, int buff_len);

static void loading_callback(void *user_data, int ret);
static void convert_snap_handler();

static volatile int terminate;
static PICAM360CAPTURE_T _state, *state = &_state;

static float lg_cam_fps[MAX_CAM_NUM] = { };
static float lg_cam_frameskip[MAX_CAM_NUM] = { };
static float lg_cam_bandwidth = 0;


static json_t *json_load_file_without_comment(const char *path, size_t flags, json_error_t *error) {
	json_t *options;
	char buff[1024];
	char *tmp_conf_filepath = "/tmp/picam360-capture.conf.json";
	snprintf(buff, sizeof(buff), "grep -v -e '^\s*#' %s > %s", path, tmp_conf_filepath);
	system(buff);
	options = json_load_file(tmp_conf_filepath, flags, error);
	return options;
}

static int end_width(const char *str, const char *suffix) {
	if (!str || !suffix)
		return 0;
	size_t lenstr = strlen(str);
	size_t lensuffix = strlen(suffix);
	if (lensuffix > lenstr)
		return 0;
	return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

static RENDERER_T *get_renderer(const char *renderer_string) {
	for (int i = 0; state->renderers[i] != NULL; i++) {
		if (strncasecmp(state->renderers[i]->name, renderer_string, 64) == 0) {
			return state->renderers[i];
		}
	}
	return NULL;
}

static void glfwErrorCallback(int num, const char* err_str) {
	printf("GLFW Error: %s\n", err_str);
}

/***********************************************************
 * Name: init_ogl
 *
 * Arguments:
 *       PICAM360CAPTURE_T *state - holds OGLES model info
 *
 * Description: Sets the display, OpenGL|ES context and screen stuff
 *
 * Returns: void
 *
 ***********************************************************/
static void init_ogl(PICAM360CAPTURE_T *state) {
#ifdef USE_GLES
#ifdef BCM_HOST
	int32_t success = 0;
	EGLBoolean result;
	EGLint num_config;

	static EGL_DISPMANX_WINDOW_T nativewindow;

	DISPMANX_ELEMENT_HANDLE_T dispman_element;
	DISPMANX_DISPLAY_HANDLE_T dispman_display;
	DISPMANX_UPDATE_HANDLE_T dispman_update;
	VC_RECT_T dst_rect;
	VC_RECT_T src_rect;

	static const EGLint attribute_list[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16,
		//EGL_SAMPLES, 4,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
	static const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

	EGLConfig config;

	// get an EGL display connection
	state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	assert(state->display != EGL_NO_DISPLAY);

	// initialize the EGL display connection
	result = eglInitialize(state->display, NULL, NULL);
	assert(EGL_FALSE != result);

	// get an appropriate EGL frame buffer configuration
	// this uses a BRCM extension that gets the closest match, rather than standard which returns anything that matches
	result = eglSaneChooseConfigBRCM(state->display, attribute_list, &config, 1, &num_config);
	assert(EGL_FALSE != result);

	//Bind to the right EGL API.
	result = eglBindAPI(EGL_OPENGL_ES_API);
	assert(result != EGL_FALSE);

	// create an EGL rendering context
	state->context = eglCreateContext(state->display, config, EGL_NO_CONTEXT, context_attributes);
	assert(state->context != EGL_NO_CONTEXT);

	// create an EGL window surface
	success = graphics_get_display_size(0 /* LCD */, &state->screen_width, &state->screen_height);
	assert(success >= 0);

	dst_rect.x = 0;
	dst_rect.y = 0;
	dst_rect.width = state->screen_width;
	dst_rect.height = state->screen_height;

	src_rect.x = 0;
	src_rect.y = 0;
	src_rect.width = state->screen_width << 16;
	src_rect.height = state->screen_height << 16;

	//if (state->preview)
	{
		dispman_display = vc_dispmanx_display_open(0 /* LCD */);
		dispman_update = vc_dispmanx_update_start(0);

		dispman_element = vc_dispmanx_element_add(dispman_update, dispman_display, 0/*layer*/, &dst_rect, 0/*src*/, &src_rect, DISPMANX_PROTECTION_NONE, 0 /*alpha*/, 0/*clamp*/, 0/*transform*/);

		nativewindow.element = dispman_element;
		nativewindow.width = state->screen_width;
		nativewindow.height = state->screen_height;
		vc_dispmanx_update_submit_sync(dispman_update);

		state->surface = eglCreateWindowSurface(state->display, config, &nativewindow, NULL);
	}
//	else {
//		//Create an offscreen rendering surface
//		EGLint rendering_attributes[] = { EGL_WIDTH, state->screen_width,
//				EGL_HEIGHT, state->screen_height, EGL_NONE };
//		state->surface = eglCreatePbufferSurface(state->display, config,
//				rendering_attributes);
//	}
	assert(state->surface != EGL_NO_SURFACE);

	// connect the context to the surface
	result = eglMakeCurrent(state->display, state->surface, state->surface, state->context);
	assert(EGL_FALSE != result);

	// Enable back face culling.
	glEnable(GL_CULL_FACE);
#elif TEGRA
	EGLBoolean result;
	EGLint num_config;
	static const EGLint attribute_list[] = {
		EGL_RED_SIZE, 8, //
		EGL_GREEN_SIZE, 8,//
		EGL_BLUE_SIZE, 8,//
		EGL_ALPHA_SIZE, 8,//
		EGL_DEPTH_SIZE, 16,//
		//EGL_SAMPLES, 4,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,//
		EGL_NONE};
	static const EGLint context_attributes[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2, //
		EGL_NONE};

	EGLConfig config;

	// get an EGL display connection
	state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	assert(state->display != EGL_NO_DISPLAY);

	// initialize the EGL display connection
	result = eglInitialize(state->display, NULL, NULL);
	assert(EGL_FALSE != result);

	// get an appropriate EGL frame buffer configuration
	result = eglChooseConfig(state->display, attribute_list, &config, 1, &num_config);
	assert(EGL_FALSE != result);

	//Bind to the right EGL API.
	result = eglBindAPI(EGL_OPENGL_ES_API);
	assert(result != EGL_FALSE);

	// create an EGL rendering context
	state->context = eglCreateContext(state->display, config, EGL_NO_CONTEXT, context_attributes);
	assert(state->context != EGL_NO_CONTEXT);

	//Create an offscreen rendering surface
	EGLint rendering_attributes[] = {EGL_WIDTH, state->screen_width,
		EGL_HEIGHT, state->screen_height, EGL_NONE};
	state->surface = eglCreatePbufferSurface(state->display, config,
			rendering_attributes);

	// connect the context to the surface
	result = eglMakeCurrent(state->display, state->surface, state->surface, state->context);
	assert(EGL_FALSE != result);

	// Enable back face culling.
	glEnable(GL_CULL_FACE);

	state->screen_width = 640;
	state->screen_height = 480;
#endif
#else
	glfwSetErrorCallback(glfwErrorCallback);
	if (glfwInit() == GL_FALSE) {
		printf("error on glfwInit\n");
	}
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

	state->screen_width = 640;
	state->screen_height = 480;
	state->glfw_window = glfwCreateWindow(state->screen_width, state->screen_height, "picam360", NULL, NULL);

	glfwMakeContextCurrent(state->glfw_window);

	glewExperimental = GL_TRUE; //avoid glGenVertexArrays crash with glew-1.13
	if (glewInit() != GLEW_OK) {
		printf("error on glewInit\n");
	}
	glClearColor(1.0f, 1.0f, 1.0f, 0.0f);
#endif
}

static int load_texture(const char *filename, uint32_t *tex_out) {
	GLenum err;
	GLuint tex;
	IplImage *iplImage = cvLoadImage(filename, CV_LOAD_IMAGE_COLOR);

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, iplImage->width, iplImage->height, 0, GL_RGB, GL_UNSIGNED_BYTE, iplImage->imageData);
	if ((err = glGetError()) != GL_NO_ERROR) {
#ifdef USE_GLES
		printf("glTexImage2D failed. Could not allocate texture buffer.\n");
#else
		printf("glTexImage2D failed. Could not allocate texture buffer. %s\n", gluErrorString(err));
#endif
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	if (tex_out != NULL)
		*tex_out = tex;

	return 0;
}

int board_mesh(int num_of_steps, GLuint *vbo_out, GLuint *n_out, GLuint *vao_out) {
	GLuint vbo;

	int n = 2 * (num_of_steps + 1) * num_of_steps;
	float points[4 * n];

	float start_x = 0.0f;
	float start_y = 0.0f;

	float end_x = 1.0f;
	float end_y = 1.0f;

	float step_x = (end_x - start_x) / num_of_steps;
	float step_y = (end_y - start_y) / num_of_steps;

	int idx = 0;
	int i, j;
	for (i = 0; i < num_of_steps; i++) {	//x
		for (j = 0; j <= num_of_steps; j++) {	//y
			{
				float x = start_x + step_x * i;
				float y = start_y + step_y * j;
				float z = 1.0;
				points[idx++] = x;
				points[idx++] = y;
				points[idx++] = z;
				points[idx++] = 1.0;
				//printf("x=%f,y=%f,z=%f,w=%f\n", points[idx - 4],
				//		points[idx - 3], points[idx - 2], points[idx - 1]);
			}
			{
				float x = start_x + step_x * (i + 1);
				float y = start_y + step_y * j;
				float z = 1.0;
				points[idx++] = x;
				points[idx++] = y;
				points[idx++] = z;
				points[idx++] = 1.0;
				//printf("x=%f,y=%f,z=%f,w=%f\n", points[idx - 4],
				//		points[idx - 3], points[idx - 2], points[idx - 1]);
			}
		}
	}

	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * n, points, GL_STATIC_DRAW);

#ifdef USE_GLES
#else
	GLuint vao;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	glBindVertexArray(0);

	if (vao_out != NULL)
		*vao_out = vao;
#endif

	if (vbo_out != NULL)
		*vbo_out = vbo;
	if (n_out != NULL)
		*n_out = n;

	return 0;
}

/***********************************************************
 * Name: init_model_proj
 *
 * Arguments:
 *       PICAM360CAPTURE_T *state - holds OGLES model info
 *
 * Description: Sets the OpenGL|ES model to default values
 *
 * Returns: void
 *
 ***********************************************************/
extern void create_calibration_renderer(PLUGIN_HOST_T *plugin_host, RENDERER_T **out_renderer);
extern void create_board_renderer(PLUGIN_HOST_T *plugin_host, RENDERER_T **out_renderer);
static void init_model_proj(PICAM360CAPTURE_T *state) {
#ifdef USE_GLES
	char *common = "#version 100\n";
#else
	char *common = "#version 330\n";
#endif

	{
		RENDERER_T *renderer = NULL;
		create_calibration_renderer(&state->plugin_host, &renderer);
		state->plugin_host.add_renderer(renderer);
	}
	{
		RENDERER_T *renderer = NULL;
		create_board_renderer(&state->plugin_host, &renderer);
		state->plugin_host.add_renderer(renderer);
	}
}

/***********************************************************
 * Name: init_textures
 *
 * Arguments:
 *       PICAM360CAPTURE_T *state - holds OGLES model info
 *
 * Description:   Initialise OGL|ES texture surfaces to use image
 *                buffers
 *
 * Returns: void
 *
 ***********************************************************/
static void deinit_textures(PICAM360CAPTURE_T *state) {
	for (int i = 0; i < state->num_of_cam; i++) {
		for (int j = 0; j < TEXTURE_BUFFER_NUM; j++) {
			if (state->cam_texture[i][j] != 0) {
				glBindTexture(GL_TEXTURE_2D, 0);
				glDeleteTextures(1, &state->cam_texture[i][j]);
				state->cam_texture[i][j] = 0;
			}
		}
	}
}
static void init_textures(PICAM360CAPTURE_T *state) {

	{
		const char *tmp_filepath = "/tmp/tmp.png";
		int *fd = open(tmp_filepath, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IXOTH);
		write(fd, logo_png, logo_png_len);
		close(fd);
		load_texture(tmp_filepath, &state->logo_texture);
		remove(tmp_filepath);
	}

	for (int i = 0; i < state->num_of_cam; i++) {
		for (int j = 0; j < TEXTURE_BUFFER_NUM; j++) {
			glGenTextures(1, &state->cam_texture[i][j]);

			glBindTexture(GL_TEXTURE_2D, state->cam_texture[i][j]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, state->cam_width, state->cam_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
	}

	for (int i = 0; i < state->num_of_cam; i++) {
		for (int j = 0; state->capture_factories[j] != NULL; j++) {
			if (strncmp(state->capture_factories[j]->name, state->capture_name, sizeof(state->capture_name)) == 0) {
				state->capture_factories[j]->create_capture(state->capture_factories[j], &state->captures[i]);
				break;
			}
		}
		if (state->captures[i]) {
#ifdef USE_GLES
			state->captures[i]->start(state->captures[i], i, state->display, state->context, state->cam_texture[i], TEXTURE_BUFFER_NUM);
#else
			glfwWindowHint(GLFW_VISIBLE, GL_FALSE);
			GLFWwindow *win = glfwCreateWindow(1, 1, "dummy window", 0, state->glfw_window);
			state->captures[i]->start(state->captures[i], i, win, NULL, NULL, TEXTURE_BUFFER_NUM);
#endif
		}
	}
	for (int i = 0; i < state->num_of_cam; i++) {
		for (int j = 0; state->decoder_factories[j] != NULL; j++) {
			if (strncmp(state->decoder_factories[j]->name, state->decoder_name, sizeof(state->decoder_name)) == 0) {
				state->decoder_factories[j]->create_decoder(state->decoder_factories[j], &state->decoders[i]);
				break;
			}
		}
		if (state->decoders[i]) {
#ifdef USE_GLES
			state->decoders[i]->init(state->decoders[i], i, state->display, state->context, state->cam_texture[i], TEXTURE_BUFFER_NUM);
#else
			glfwWindowHint(GLFW_VISIBLE, GL_FALSE);
			GLFWwindow *win = glfwCreateWindow(1, 1, "dummy window", 0, state->glfw_window);
			state->decoders[i]->init(state->decoders[i], i, win, NULL, state->cam_texture[i], TEXTURE_BUFFER_NUM);
#endif
		}
	}
	{
		for (int j = 0; state->capture_factories[j] != NULL; j++) {
			if (strncmp(state->capture_factories[j]->name, state->audio_capture_name, sizeof(state->audio_capture_name)) == 0) {
				state->capture_factories[j]->create_capture(state->capture_factories[j], &state->audio_capture);
				break;
			}
		}
		if (state->audio_capture) {
			state->audio_capture->start(state->audio_capture, 0, NULL, NULL, NULL, 0);
		}
	}
}
//------------------------------------------------------------------------------

/***********************************************************
 * Name: init_options
 *
 * Arguments:
 *       PICAM360CAPTURE_T *state - holds OGLES model info
 *
 * Description:   Initialise options
 *
 * Returns: void
 *
 ***********************************************************/
static void init_options(PICAM360CAPTURE_T *state) {
	json_error_t error;
	json_t *options = json_load_file_without_comment(state->config_filepath, 0, &error);
	if (options == NULL) {
		fputs(error.text, stderr);
	} else {
		state->num_of_cam = json_number_value(json_object_get(options, "num_of_cam"));
		state->num_of_cam = MAX(1, MIN(state->num_of_cam, MAX_CAM_NUM));
		{
			json_t *value = json_object_get(options, "mpu_name");
			if (value) {
				strncpy(state->mpu_name, json_string_value(value), sizeof(state->mpu_name) - 1);
			}
		}
		{
			json_t *value = json_object_get(options, "capture_name");
			if (value) {
				strncpy(state->capture_name, json_string_value(value), sizeof(state->capture_name) - 1);
			}
		}
		{
			json_t *value = json_object_get(options, "decoder_name");
			if (value) {
				strncpy(state->decoder_name, json_string_value(value), sizeof(state->decoder_name) - 1);
			}
		}
		{
			json_t *value = json_object_get(options, "audio_capture_name");
			if (value) {
				strncpy(state->audio_capture_name, json_string_value(value), sizeof(state->audio_capture_name) - 1);
			}
		}
		state->options.sharpness_gain = json_number_value(json_object_get(options, "sharpness_gain"));
		state->options.color_offset = json_number_value(json_object_get(options, "color_offset"));
		state->options.overlap = json_number_value(json_object_get(options, "overlap"));
		for (int i = 0; i < state->num_of_cam; i++) {
			char buff[256];
			sprintf(buff, "cam%d_offset_pitch", i);
			state->options.cam_offset_pitch[i] = json_number_value(json_object_get(options, buff));
			sprintf(buff, "cam%d_offset_yaw", i);
			state->options.cam_offset_yaw[i] = json_number_value(json_object_get(options, buff));
			sprintf(buff, "cam%d_offset_roll", i);
			state->options.cam_offset_roll[i] = json_number_value(json_object_get(options, buff));
			sprintf(buff, "cam%d_offset_x", i);
			state->options.cam_offset_x[i] = json_number_value(json_object_get(options, buff));
			sprintf(buff, "cam%d_offset_y", i);
			state->options.cam_offset_y[i] = json_number_value(json_object_get(options, buff));
			sprintf(buff, "cam%d_horizon_r", i);
			state->options.cam_horizon_r[i] = json_number_value(json_object_get(options, buff));
			sprintf(buff, "cam%d_aov", i);
			state->options.cam_aov[i] = json_number_value(json_object_get(options, buff));

			if (state->options.cam_horizon_r[i] == 0) {
				state->options.cam_horizon_r[i] = 0.8;
			}
			if (state->options.cam_aov[i] == 0) {
				state->options.cam_aov[i] = 245;
			}
		}
		{ //rtp
			state->options.rtp_rx_port = json_number_value(json_object_get(options, "rtp_rx_port"));
			state->options.rtp_rx_type = rtp_get_rtp_socket_type(json_string_value(json_object_get(options, "rtp_rx_type")));
			json_t *value = json_object_get(options, "rtp_tx_ip");
			if (value) {
				strncpy(state->options.rtp_tx_ip, json_string_value(value), sizeof(state->options.rtp_tx_ip) - 1);
			} else {
				memset(state->options.rtp_tx_ip, 0, sizeof(state->options.rtp_tx_ip));
			}
			state->options.rtp_tx_port = json_number_value(json_object_get(options, "rtp_tx_port"));
			state->options.rtp_tx_type = rtp_get_rtp_socket_type(json_string_value(json_object_get(options, "rtp_tx_type")));
		}
		{ //rtcp
			state->options.rtcp_rx_port = json_number_value(json_object_get(options, "rtcp_rx_port"));
			state->options.rtcp_rx_type = rtp_get_rtp_socket_type(json_string_value(json_object_get(options, "rtcp_rx_type")));
			json_t *value = json_object_get(options, "rtcp_tx_ip");
			if (value) {
				strncpy(state->options.rtcp_tx_ip, json_string_value(value), sizeof(state->options.rtcp_tx_ip) - 1);
			} else {
				memset(state->options.rtcp_tx_ip, 0, sizeof(state->options.rtcp_tx_ip));
			}
			state->options.rtcp_tx_port = json_number_value(json_object_get(options, "rtcp_tx_port"));
			state->options.rtcp_tx_type = rtp_get_rtp_socket_type(json_string_value(json_object_get(options, "rtcp_tx_type")));
		}

		json_decref(options);
	}
}
//------------------------------------------------------------------------------

/***********************************************************
 * Name: save_options
 *
 * Arguments:
 *       PICAM360CAPTURE_T *state - holds OGLES model info
 *
 * Description:   Initialise options
 *
 * Returns: void
 *
 ***********************************************************/
static void save_options(PICAM360CAPTURE_T *state) {
	json_t *options = json_object();

	json_object_set_new(options, "num_of_cam", json_integer(state->num_of_cam));
	json_object_set_new(options, "mpu_name", json_string(state->mpu_name));
	json_object_set_new(options, "capture_name", json_string(state->capture_name));
	json_object_set_new(options, "decoder_name", json_string(state->decoder_name));
	json_object_set_new(options, "audio_capture_name", json_string(state->audio_capture_name));
	json_object_set_new(options, "sharpness_gain", json_real(state->options.sharpness_gain));
	json_object_set_new(options, "color_offset", json_real(state->options.color_offset));
	json_object_set_new(options, "overlap", json_real(state->options.overlap));
	for (int i = 0; i < state->num_of_cam; i++) {
		char buff[256];
		sprintf(buff, "cam%d_offset_pitch", i);
		json_object_set_new(options, buff, json_real(state->options.cam_offset_pitch[i]));
		sprintf(buff, "cam%d_offset_yaw", i);
		json_object_set_new(options, buff, json_real(state->options.cam_offset_yaw[i]));
		sprintf(buff, "cam%d_offset_roll", i);
		json_object_set_new(options, buff, json_real(state->options.cam_offset_roll[i]));
		sprintf(buff, "cam%d_offset_x", i);
		json_object_set_new(options, buff, json_real(state->options.cam_offset_x[i]));
		sprintf(buff, "cam%d_offset_y", i);
		json_object_set_new(options, buff, json_real(state->options.cam_offset_y[i]));
		sprintf(buff, "cam%d_horizon_r", i);
		json_object_set_new(options, buff, json_real(state->options.cam_horizon_r[i]));
		sprintf(buff, "cam%d_aov", i);
		json_object_set_new(options, buff, json_real(state->options.cam_aov[i]));
	}
	{ //rtp
		json_object_set_new(options, "rtp_rx_port", json_integer(state->options.rtp_rx_port));
		json_object_set_new(options, "rtp_rx_type", json_string(rtp_get_rtp_socket_type_str(state->options.rtp_rx_type)));
		json_object_set_new(options, "rtp_tx_ip", json_string(state->options.rtp_tx_ip));
		json_object_set_new(options, "rtp_tx_port", json_integer(state->options.rtp_tx_port));
		json_object_set_new(options, "rtp_tx_type", json_string(rtp_get_rtp_socket_type_str(state->options.rtp_tx_type)));
	}
	{ //rtcp
		json_object_set_new(options, "rtcp_rx_port", json_integer(state->options.rtcp_rx_port));
		json_object_set_new(options, "rtcp_rx_type", json_string(rtp_get_rtp_socket_type_str(state->options.rtcp_rx_type)));
		json_object_set_new(options, "rtcp_tx_ip", json_string(state->options.rtcp_tx_ip));
		json_object_set_new(options, "rtcp_tx_port", json_integer(state->options.rtcp_tx_port));
		json_object_set_new(options, "rtcp_tx_type", json_string(rtp_get_rtp_socket_type_str(state->options.rtcp_tx_type)));
	}

	if (state->plugin_paths) {
		json_t *plugin_paths = json_array();
		for (int i = 0; state->plugin_paths[i] != NULL; i++) {
			json_array_append_new(plugin_paths, json_string(state->plugin_paths[i]));
		}
		json_object_set_new(options, "plugin_paths", plugin_paths);
	}

	for (int i = 0; state->plugins[i] != NULL; i++) {
		if (state->plugins[i]->save_options) {
			state->plugins[i]->save_options(state->plugins[i]->user_data, options);
		}
	}

	json_dump_file(options, state->config_filepath, JSON_PRESERVE_ORDER | JSON_INDENT(4) | JSON_REAL_PRECISION(9));

	json_decref(options);
}

static void init_options_ex(PICAM360CAPTURE_T *state) {
	json_error_t error;
	json_t *options = json_load_file(state->options.config_ex_filepath, 0, &error);
	if (options != NULL) {
		for (int i = 0; i < state->num_of_cam; i++) {
			char buff[256];
			sprintf(buff, "cam%d_offset_x", i);
			state->options.cam_offset_x_ex[i] = json_number_value(json_object_get(options, buff));
			sprintf(buff, "cam%d_offset_y", i);
			state->options.cam_offset_y_ex[i] = json_number_value(json_object_get(options, buff));
			sprintf(buff, "cam%d_horizon_r", i);
			state->options.cam_horizon_r_ex[i] = json_number_value(json_object_get(options, buff));
		}
	}
}

static void save_options_ex(PICAM360CAPTURE_T *state) {
	json_t *options = json_object();

	for (int i = 0; i < state->num_of_cam; i++) {
		char buff[256];
		sprintf(buff, "cam%d_offset_x", i);
		json_object_set_new(options, buff, json_real(state->options.cam_offset_x_ex[i]));
		sprintf(buff, "cam%d_offset_y", i);
		json_object_set_new(options, buff, json_real(state->options.cam_offset_y_ex[i]));
		sprintf(buff, "cam%d_horizon_r", i);
		json_object_set_new(options, buff, json_real(state->options.cam_horizon_r_ex[i]));
	}

	json_dump_file(options, state->options.config_ex_filepath, JSON_INDENT(4));

	json_decref(options);
}
//------------------------------------------------------------------------------

static void exit_func(void)
// Function to be passed to atexit().
{
#ifdef USE_GLES
	deinit_textures(state);

	// clear screen
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(state->display, state->surface);

	// Release OpenGL resources
	eglMakeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(state->display, state->surface);
	eglDestroyContext(state->display, state->context);
	eglTerminate(state->display);
#endif
	printf("\npicam360-capture closed\n");
} // exit_func()

//==============================================================================

bool inputAvailable() {
	struct timeval tv;
	fd_set fds;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
	return (FD_ISSET(0, &fds));
}

FRAME_T *create_frame(PICAM360CAPTURE_T *state, int argc, char *argv[]) {
	GLenum err;
	int opt;
	int render_width = 512;
	int render_height = 512;
	FRAME_T *frame = malloc(sizeof(FRAME_T));
	memset(frame, 0, sizeof(FRAME_T));
	frame->id = state->next_frame_id++;
	frame->renderer = state->renderers[0];
	frame->output_mode = OUTPUT_MODE_NONE;
	frame->output_type = OUTPUT_TYPE_NONE;
	frame->output_fd = -1;
	frame->fov = 120;

	optind = 1; // reset getopt
	while ((opt = getopt(argc, argv, "w:h:m:o:s:v:f:k:")) != -1) {
		switch (opt) {
		case 'w':
			sscanf(optarg, "%d", &render_width);
			break;
		case 'h':
			sscanf(optarg, "%d", &render_height);
			break;
		case 'm':
			frame->renderer = get_renderer(optarg);
			if (frame->renderer == NULL) {
				printf("%s renderer is not existing.\n", optarg);
			}
			break;
		case 'o':
			frame->output_mode = OUTPUT_MODE_VIDEO;
			strncpy(frame->output_filepath, optarg, sizeof(frame->output_filepath));
			break;
		case 'v':
			for (int i = 0; state->mpu_factories[i] != NULL; i++) {
				if (strncmp(state->mpu_factories[i]->name, optarg, 64) == 0) {
					state->mpu_factories[i]->create_mpu(state->mpu_factories[i]->user_data, &frame->view_mpu);
				}
			}
			break;
		case 's':
			frame->output_mode = OUTPUT_MODE_STREAM;
			for (int i = 0; state->encoder_factories[i] != NULL; i++) {
				if (strncmp(state->encoder_factories[i]->name, optarg, sizeof(frame->encoder)) == 0) {
					state->encoder_factories[i]->create_encoder(state->encoder_factories[i]->user_data, &frame->encoder);
				}
			}
			if (frame->encoder == NULL) {
				printf("%s is not supported\n", optarg);
			}
			//h264 or mjpeg
			if (strcasecmp(optarg, "h265") == 0) {
				frame->output_type = OUTPUT_TYPE_H265;
			} else if (strcasecmp(optarg, "h264") == 0) {
				frame->output_type = OUTPUT_TYPE_H264;
			} else {
				frame->output_type = OUTPUT_TYPE_MJPEG;
			}
			break;
		case 'f':
			sscanf(optarg, "%f", &frame->fps);
			break;
		case 'k':
			sscanf(optarg, "%f", &frame->kbps);
			break;
		default:
			break;
		}
	}

	if (frame->view_mpu == NULL) {
		for (int i = 0; state->mpu_factories[i] != NULL; i++) {
			if (strncmp(state->mpu_factories[i]->name, state->default_view_coordinate_mode, 64) == 0) {
				state->mpu_factories[i]->create_mpu(state->mpu_factories[i]->user_data, &frame->view_mpu);
			}
		}
	}

	if (render_width > 2048) {
		frame->double_size = true;
		frame->width = render_width / 2;
		frame->height = render_height;
	} else {
		frame->width = render_width;
		frame->height = render_height;
	}

	//texture rendering
	glGenFramebuffers(1, &frame->framebuffer);

	glGenTextures(1, &frame->texture);
	glBindTexture(GL_TEXTURE_2D, frame->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame->width, frame->height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	if ((err = glGetError()) != GL_NO_ERROR) {
#ifdef USE_GLES
		printf("glTexImage2D failed. Could not allocate texture buffer.\n");
#else
		printf("glTexImage2D failed. Could not allocate texture buffer.\n %s\n", gluErrorString(err));
#endif
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, frame->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frame->texture, 0);
	if ((err = glGetError()) != GL_NO_ERROR) {
#ifdef USE_GLES
		printf("glFramebufferTexture2D failed. Could not allocate framebuffer.\n");
#else
		printf("glFramebufferTexture2D failed. Could not allocate framebuffer. %s\n", gluErrorString(err));
#endif
	}

	// Set background color and clear buffers
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	//buffer memory
	if (frame->double_size) {
		int size = frame->width * frame->height * 3;
		frame->img_buff = (unsigned char*) malloc(size * 2);
	} else {
		int size = frame->width * frame->height * 3;
		frame->img_buff = (unsigned char*) malloc(size);
	}

	printf("create_frame id=%d\n", frame->id);

	return frame;
}

bool delete_frame(FRAME_T *frame) {
	printf("delete_frame id=%d\n", frame->id);

	if (frame->befor_deleted_callback) {
		frame->befor_deleted_callback(state, frame);
	}

	if (frame->framebuffer) {
		glDeleteFramebuffers(1, &frame->framebuffer);
		frame->framebuffer = 0;
	}
	if (frame->texture) {
		glDeleteTextures(1, &frame->texture);
		frame->texture = 0;
	}
	if (frame->img_buff) {
		free(frame->img_buff);
		frame->img_buff = NULL;
	}
	if (frame->view_mpu) {
		frame->view_mpu->release(frame->view_mpu);
		frame->view_mpu = NULL;
	}

	if (frame->encoder) { //stop record
		frame->encoder->release(frame->encoder);
		frame->encoder = NULL;
	}

	if (frame->output_fd >= 0) {
		close(frame->output_fd);
		frame->output_fd = -1;
	}

	free(frame);

	return true;
}

static void stream_callback(unsigned char *data, unsigned int data_len, void *frame_data, void *user_data);

void frame_handler() {
	struct timeval s, f;
	double elapsed_ms;
	bool snap_finished = false;
	FRAME_T **frame_pp = &state->frame;
	while (*frame_pp) {
		FRAME_INFO_T frame_info;
		FRAME_T *frame = *frame_pp;
		gettimeofday(&s, NULL);

		if (frame->fps > 0) {
			struct timeval diff;
			timersub(&s, &frame->last_updated, &diff);
			float diff_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;
			if (diff_sec < 1.0 / frame->fps) {
				frame_pp = &frame->next;
				continue;
			}
		}

		//start & stop recording
		if (frame->is_recording && frame->output_mode == OUTPUT_MODE_NONE) { //stop record
			frame->encoder->release(frame->encoder);
			frame->encoder = NULL;

			frame->frame_elapsed /= frame->frame_num;
			printf("stop record : frame num : %d : fps %.3lf\n", frame->frame_num, 1000.0 / frame->frame_elapsed);

			frame->output_mode = OUTPUT_MODE_NONE;
			frame->is_recording = false;
			frame->delete_after_processed = true;
			if (frame->output_fd >= 0) {
				close(frame->output_fd);
				frame->output_fd = -1;
			}
		}
		if (!frame->is_recording && frame->output_mode == OUTPUT_MODE_VIDEO) {
			int ratio = frame->double_size ? 2 : 1;
			float fps = MAX(frame->fps, 1);
			frame->encoder->init(frame->encoder, frame->width * ratio, frame->height, 4000 * ratio, fps, stream_callback, NULL);
			frame->frame_num = 0;
			frame->frame_elapsed = 0;
			frame->is_recording = true;
			frame->output_start = false;
			frame->output_fd = open(frame->output_filepath, O_CREAT | O_WRONLY | O_TRUNC, /*  */
			S_IRUSR | S_IWUSR | /* rw */
			S_IRGRP | S_IWGRP | /* rw */
			S_IROTH | S_IXOTH);
			printf("start_record saved to %s\n", frame->output_filepath);
		}
		if (!frame->is_recording && frame->output_mode == OUTPUT_MODE_STREAM) {
			int ratio = frame->double_size ? 2 : 1;
			float fps = MAX(frame->fps, 1);
			float kbps = frame->kbps;
			if (kbps == 0) {
				float ave_sq = sqrt((float) frame->width * (float) frame->height) / 1.2;
				if (frame->output_type == OUTPUT_TYPE_H265) {
					if (ave_sq <= 240) {
						kbps = 100;
					} else if (ave_sq <= 320) {
						kbps = 200;
					} else if (ave_sq <= 480) {
						kbps = 400;
					} else if (ave_sq <= 640) {
						kbps = 800;
					} else if (ave_sq <= 960) {
						kbps = 1600;
					} else {
						kbps = 3200;
					}
				} else if (frame->output_type == OUTPUT_TYPE_H264) {
					if (ave_sq <= 240) {
						kbps = 200;
					} else if (ave_sq <= 320) {
						kbps = 400;
					} else if (ave_sq <= 480) {
						kbps = 800;
					} else if (ave_sq <= 640) {
						kbps = 1600;
					} else if (ave_sq <= 960) {
						kbps = 3200;
					} else {
						kbps = 6400;
					}
				} else {
					if (ave_sq <= 240) {
						kbps = 800;
					} else if (ave_sq <= 320) {
						kbps = 1600;
					} else if (ave_sq <= 480) {
						kbps = 3200;
					} else if (ave_sq <= 640) {
						kbps = 6400;
					} else if (ave_sq <= 960) {
						kbps = 12800;
					} else {
						kbps = 25600;
					}
				}
			}
			frame->encoder->init(frame->encoder, frame->width * ratio, frame->height, kbps * ratio, fps, stream_callback, frame);
			frame->frame_num = 0;
			frame->frame_elapsed = 0;
			frame->is_recording = true;
			printf("start_record saved to %s : %d kbps\n", frame->output_filepath, (int) kbps);
		}

		{ //rendering to buffer
			VECTOR4D_T view_quat = { .ary = { 0, 0, 0, 1 } };
			if (frame->view_mpu) {
				view_quat = frame->view_mpu->get_quaternion(frame->view_mpu);
			}

			{ //store info
				memcpy(frame_info.client_key, frame->client_key, sizeof(frame_info.client_key));
				frame_info.server_key = frame->server_key;
				gettimeofday(&frame_info.before_redraw_render_texture, NULL);
				frame_info.fov = frame->fov;
				frame_info.view_quat = view_quat;
			}

			state->plugin_host.lock_texture();
			if (frame->double_size) {
				int size = frame->width * frame->height * 3;
				unsigned char *image_buffer = (unsigned char*) malloc(size);
				unsigned char *image_buffer_double = frame->img_buff;
				frame->img_width = frame->width * 2;
				frame->img_height = frame->height;
				for (int split = 0; split < 2; split++) {
					state->split = split + 1;

					glBindFramebuffer(GL_FRAMEBUFFER, frame->framebuffer);
					redraw_render_texture(state, frame, frame->renderer, view_quat);
					glFinish();
					glReadPixels(0, 0, frame->width, frame->height, GL_RGB, GL_UNSIGNED_BYTE, image_buffer);
					glBindFramebuffer(GL_FRAMEBUFFER, 0);
					for (int y = 0; y < frame->height; y++) {
						memcpy(image_buffer_double + frame->width * 2 * 3 * y + frame->width * 3 * split, image_buffer + frame->width * 3 * y, frame->width * 3);
					}
				}
				free(image_buffer);
			} else {
				unsigned char *image_buffer = frame->img_buff;
				frame->img_width = frame->width;
				frame->img_height = frame->height;
				state->split = 0;

				glBindFramebuffer(GL_FRAMEBUFFER, frame->framebuffer);
				redraw_render_texture(state, frame, frame->renderer, view_quat);
				if (state->menu_visible) {
					redraw_info(state, frame);
				}
				glFinish();
				glReadPixels(0, 0, frame->width, frame->height, GL_RGB, GL_UNSIGNED_BYTE, image_buffer);
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
			}
			state->plugin_host.unlock_texture();

			{ //store info
				gettimeofday(&frame_info.after_redraw_render_texture, NULL);
			}
		}

		switch (frame->output_mode) {
		case OUTPUT_MODE_STILL:
#if(0)
			SaveJpeg(frame->img_buff, frame->img_width, frame->img_height, frame->output_filepath, 70);
			printf("snap saved to %s\n", frame->output_filepath);

			gettimeofday(&f, NULL);
			elapsed_ms = (f.tv_sec - s.tv_sec) * 1000.0 + (f.tv_usec - s.tv_usec) / 1000.0;
			printf("elapsed %.3lf ms\n", elapsed_ms);
#endif
			frame->output_mode = OUTPUT_MODE_NONE;
			frame->delete_after_processed = true;

			snap_finished = true;
			break;
		case OUTPUT_MODE_VIDEO:
			if (frame->output_fd > 0) {
				frame->encoder->add_frame(frame->encoder, frame->img_buff, NULL);

				gettimeofday(&f, NULL);
				elapsed_ms = (f.tv_sec - s.tv_sec) * 1000.0 + (f.tv_usec - s.tv_usec) / 1000.0;
				frame->frame_num++;
				frame->frame_elapsed += elapsed_ms;
			} else {
				frame->output_mode = OUTPUT_MODE_NONE;
				frame->delete_after_processed = true;
			}
			break;
		case OUTPUT_MODE_STREAM:
			if (1) {
				FRAME_INFO_T *frame_info_p = malloc(sizeof(FRAME_INFO_T));
				memcpy(frame_info_p, &frame_info, sizeof(FRAME_INFO_T));
				frame->encoder->add_frame(frame->encoder, frame->img_buff, frame_info_p);
			}

			gettimeofday(&f, NULL);
			elapsed_ms = (f.tv_sec - s.tv_sec) * 1000.0 + (f.tv_usec - s.tv_usec) / 1000.0;
			frame->frame_num++;
			frame->frame_elapsed += elapsed_ms;

			if (end_width(frame->output_filepath, ".jpeg")) {
#if(0)
				SaveJpeg(frame->img_buff, frame->img_width, frame->img_height, frame->output_filepath, 70);
				printf("snap saved to %s\n", frame->output_filepath);
#endif
				frame->output_filepath[0] = '\0';
			} else if (end_width(frame->output_filepath, ".h265")) {
				if (frame->output_type == OUTPUT_TYPE_H265) {
					frame->output_start = false;
					frame->output_fd = open(frame->output_filepath, O_CREAT | O_WRONLY | O_TRUNC, /*  */
					S_IRUSR | S_IWUSR | /* rw */
					S_IRGRP | S_IWGRP | /* rw */
					S_IROTH | S_IXOTH);
					printf("start record to %s\n", frame->output_filepath);
				} else {
					printf("error type : %s\n", frame->output_filepath);
				}
				frame->output_filepath[0] = '\0';
			} else if (end_width(frame->output_filepath, ".h264")) {
				if (frame->output_type == OUTPUT_TYPE_H264) {
					frame->output_start = false;
					frame->output_fd = open(frame->output_filepath, O_CREAT | O_WRONLY | O_TRUNC, /*  */
					S_IRUSR | S_IWUSR | /* rw */
					S_IRGRP | S_IWGRP | /* rw */
					S_IROTH | S_IXOTH);
					printf("start record to %s\n", frame->output_filepath);
				} else {
					printf("error type : %s\n", frame->output_filepath);
				}
				frame->output_filepath[0] = '\0';
			} else if (end_width(frame->output_filepath, ".mjpeg")) {
				if (frame->output_type == OUTPUT_TYPE_MJPEG) {
					frame->output_start = false;
					frame->output_fd = open(frame->output_filepath, O_CREAT | O_WRONLY | O_TRUNC, /*  */
					S_IRUSR | S_IWUSR | /* rw */
					S_IRGRP | S_IWGRP | /* rw */
					S_IROTH | S_IXOTH);
					printf("start record to %s\n", frame->output_filepath);
				} else {
					printf("error type : %s\n", frame->output_filepath);
				}
				frame->output_filepath[0] = '\0';
			}
			break;
		default:
			break;
		}
		if (frame->after_processed_callback) {
			frame->after_processed_callback(state, frame);
		}
		//next rendering
		if (frame->delete_after_processed) {
			*frame_pp = frame->next;
			delete_frame(frame);
			frame = NULL;
		} else {
			frame_pp = &frame->next;
		}
		//preview
		if (frame && frame == state->frame && state->preview) {
			state->plugin_host.lock_texture();
			{
				redraw_scene(state, frame, &state->model_data[OPERATION_MODE_BOARD]);
			}
			state->plugin_host.unlock_texture();
#ifdef USE_GLES
			eglSwapBuffers(state->display, state->surface);
#else
			glfwSwapBuffers(state->glfw_window);
			glfwPollEvents();
#endif
		}
		if (frame) {
			frame->last_updated = s;
		}
	}
	if (snap_finished) {
		state->plugin_host.send_event(PICAM360_HOST_NODE_ID, PICAM360_CAPTURE_EVENT_AFTER_SNAP);
	}
	state->plugin_host.send_event(PICAM360_HOST_NODE_ID, PICAM360_CAPTURE_EVENT_AFTER_FRAME);
}

static double calib_step = 0.01;

int _command_handler(const char *_buff) {
	int opt;
	int ret = 0;
	char buff[256];
	strncpy(buff, _buff, sizeof(buff));
	char *cmd = strtok(buff, " \n");
	if (cmd == NULL) {
		//do nothing
	} else if (strncmp(cmd, "exit", sizeof(buff)) == 0 || strncmp(cmd, "q", sizeof(buff)) == 0 || strncmp(cmd, "quit", sizeof(buff)) == 0) {
		printf("exit\n");
		exit(0);
	} else if (strncmp(cmd, "save", sizeof(buff)) == 0) {
		save_options(state);
		{ //send upstream
			char cmd[256];
			sprintf(cmd, "upstream.save");
			state->plugin_host.send_command(cmd);
		}
	} else if (cmd[1] == '\0' && cmd[0] >= '0' && cmd[0] <= '9') {
		state->active_cam = cmd[0] - '0';
	} else if (strncmp(cmd, "snap", sizeof(buff)) == 0) {
		char *param = strtok(NULL, "\n");
		if (param != NULL) {
			int id = -1;
			const int kMaxArgs = 32;
			int argc = 1;
			char *argv[kMaxArgs];
			char *p2 = strtok(param, " ");
			while (p2 && argc < kMaxArgs - 1) {
				argv[argc++] = p2;
				p2 = strtok(0, " ");
			}
			argv[0] = cmd;
			argv[argc] = 0;
			optind = 1; // reset getopt
			while ((opt = getopt(argc, argv, "i:o")) != -1) {
				switch (opt) {
				case 'i':
					sscanf(optarg, "%d", &id);
					break;
				}
			}
			if (id >= 0) {
				optind = 1; // reset getopt
				while ((opt = getopt(argc, argv, "i:o:")) != -1) {
					switch (opt) {
					case 'o':
						for (FRAME_T *frame = state->frame; frame != NULL; frame = frame->next) {
							if (frame->id == id) {
								strncpy(frame->output_filepath, optarg, sizeof(frame->output_filepath));
								printf("snap %d : %s\n", frame->id, frame->output_filepath);
								break;
							}
						}
						break;
					}
				}
			}
		}
	} else if (strncmp(cmd, "create_frame", sizeof(buff)) == 0) {
		char *param = strtok(NULL, "\n");
		if (param != NULL) {
			const int kMaxArgs = 32;
			int argc = 1;
			char *argv[kMaxArgs];
			char *p2 = strtok(param, " ");
			while (p2 && argc < kMaxArgs - 1) {
				argv[argc++] = p2;
				p2 = strtok(0, " ");
			}
			argv[0] = cmd;
			argv[argc] = 0;

			FRAME_T *frame = create_frame(state, argc, argv);
			frame->next = state->frame;
			state->frame = frame;
		}
	} else if (strncmp(cmd, "delete_frame", sizeof(buff)) == 0) {
		char *param = strtok(NULL, "\n");
		if (param != NULL) {
			bool delete_all = false;
			int id = -1;
			const int kMaxArgs = 32;
			int argc = 1;
			char *argv[kMaxArgs];
			char *p2 = strtok(param, " ");
			while (p2 && argc < kMaxArgs - 1) {
				argv[argc++] = p2;
				p2 = strtok(0, " ");
			}
			argv[0] = cmd;
			argv[argc] = 0;
			optind = 1; // reset getopt
			while ((opt = getopt(argc, argv, "i:")) != -1) {
				switch (opt) {
				case 'i':
					if (optarg[0] == '*') {
						delete_all = true;
					} else {
						sscanf(optarg, "%d", &id);
					}
					break;
				}
			}
			for (FRAME_T *frame_p = state->frame; frame_p != NULL; frame_p = frame_p->next) {
				if (delete_all || frame_p->id == id) {
					frame_p->delete_after_processed = true;
				}
			}
		}
	} else if (strncmp(cmd, "set_fps", sizeof(buff)) == 0) {
		char *param = strtok(NULL, "\n");
		if (param != NULL) {
			int id = -1;
			float value = 5;
			const int kMaxArgs = 32;
			int argc = 1;
			char *argv[kMaxArgs];
			char *p2 = strtok(param, " ");
			while (p2 && argc < kMaxArgs - 1) {
				argv[argc++] = p2;
				p2 = strtok(0, " ");
			}
			argv[0] = cmd;
			argv[argc] = 0;
			optind = 1; // reset getopt
			while ((opt = getopt(argc, argv, "f:i:")) != -1) {
				switch (opt) {
				case 'i':
					sscanf(optarg, "%d", &id);
					break;
				case 'f':
					sscanf(optarg, "%f", &value);
					break;
				}
			}
			for (FRAME_T *frame_p = state->frame; frame_p != NULL; frame_p = frame_p->next) {
				if (frame_p->id == id) {
					frame_p->fps = MAX(value, 1);
				}
			}
		}
	} else if (strncmp(cmd, "set_mode", sizeof(buff)) == 0) {
		char *param = strtok(NULL, "\n");
		if (param != NULL) {
			int id = -1;
			RENDERER_T *renderer = NULL;
			const int kMaxArgs = 32;
			int argc = 1;
			char *argv[kMaxArgs];
			char *p2 = strtok(param, " ");
			while (p2 && argc < kMaxArgs - 1) {
				argv[argc++] = p2;
				p2 = strtok(0, " ");
			}
			argv[0] = cmd;
			argv[argc] = 0;

			optind = 1; // reset getopt
			while ((opt = getopt(argc, argv, "m:i:")) != -1) {
				switch (opt) {
				case 'i':
					sscanf(optarg, "%d", &id);
					break;
				case 'm':
					renderer = get_renderer(optarg);
					break;
				}
			}
			for (FRAME_T *frame_p = state->frame; frame_p != NULL; frame_p = frame_p->next) {
				if (frame_p->id == id) {
					frame_p->renderer = renderer;
				}
			}
		}
	} else if (strncmp(cmd, "start_record", sizeof(buff)) == 0) {
		char *param = strtok(NULL, "\n");
		if (param != NULL) {
			int id = -1;
			const int kMaxArgs = 32;
			int argc = 1;
			char *argv[kMaxArgs];
			char *p2 = strtok(param, " ");
			while (p2 && argc < kMaxArgs - 1) {
				argv[argc++] = p2;
				p2 = strtok(0, " ");
			}
			argv[0] = cmd;
			argv[argc] = 0;
			optind = 1; // reset getopt
			while ((opt = getopt(argc, argv, "i:o")) != -1) {
				switch (opt) {
				case 'i':
					sscanf(optarg, "%d", &id);
					break;
				}
			}
			if (id >= 0) {
				optind = 1; // reset getopt
				while ((opt = getopt(argc, argv, "i:o:")) != -1) {
					switch (opt) {
					case 'o':
						for (FRAME_T *frame = state->frame; frame != NULL; frame = frame->next) {
							if (frame->id == id) {
								strncpy(frame->output_filepath, optarg, sizeof(frame->output_filepath));
								printf("start_record %d : %s\n", frame->id, frame->output_filepath);
								break;
							}
						}
						break;
					}
				}
			}
		}
	} else if (strncmp(cmd, "stop_record", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			int id = -1;
			int target_frame = -1;
			const int kMaxArgs = 32;
			int argc = 1;
			char *argv[kMaxArgs];
			char *p2 = strtok(param, " ");
			while (p2 && argc < kMaxArgs - 1) {
				argv[argc++] = p2;
				p2 = strtok(0, " ");
			}
			argv[0] = cmd;
			argv[argc] = 0;
			optind = 1; // reset getopt
			while ((opt = getopt(argc, argv, "i:")) != -1) {
				switch (opt) {
				case 'i':
					sscanf(optarg, "%d", &id);
					break;
				}
			}
			if (id >= 0) {
				for (FRAME_T *frame = state->frame; frame != NULL; frame = frame->next) {
					if (frame->id == target_frame) {
						if (frame->output_fd > 0) {
							close(frame->output_fd);
							frame->output_fd = -1;
							printf("stop_record %d\n", frame->id);
						} else {
							printf("error at stop_record %d\n", frame->id);
						}
						break;
					}
				}
			}
		}
	} else if (strncmp(cmd, "start_ac", sizeof(buff)) == 0) {
		bool checkAcMode = false;
		for (FRAME_T *frame = state->frame; frame != NULL; frame = frame->next) {
			if (is_auto_calibration(frame)) {
				checkAcMode = true;
				break;
			}
		}
		if (!checkAcMode) {
			char param[256];
			if (state->frame->output_mode == OUTPUT_MODE_STREAM) {
				strncpy(param, "-w 256 -h 256 -F -s mjpeg", 256);
			} else {
				strncpy(param, "-w 256 -h 256 -F", 256);
			}

			const int kMaxArgs = 32;
			int argc = 1;
			char *argv[kMaxArgs];
			char *p2 = strtok(param, " ");
			while (p2 && argc < kMaxArgs - 1) {
				argv[argc++] = p2;
				p2 = strtok(0, " ");
			}
			argv[0] = "auto_calibration";
			argv[argc] = 0;

			FRAME_T *frame = create_frame(state, argc, argv);
			set_auto_calibration(frame);
			frame->next = state->frame;
			state->frame = frame;

			printf("start_ac\n");
		}
	} else if (strncmp(cmd, "stop_ac", sizeof(buff)) == 0) {
		for (FRAME_T *frame = state->frame; frame != NULL; frame = frame->next) {
			if (is_auto_calibration(frame)) {
				frame->delete_after_processed = true;
				printf("stop_ac\n");
				break;
			}
		}
	} else if (strncmp(cmd, "start_record_raw", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL && !state->output_raw) {
			strncpy(state->output_raw_filepath, param, sizeof(state->output_raw_filepath) - 1);
			state->output_raw = true;
			printf("start_record_raw saved to %s\n", param);
		}
	} else if (strncmp(cmd, "stop_record_raw", sizeof(buff)) == 0) {
		printf("stop_record_raw\n");
		state->output_raw = false;
	} else if (strncmp(cmd, "load_file", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			strncpy(state->input_filepath, param, sizeof(state->input_filepath) - 1);
			state->input_mode = INPUT_MODE_FILE;
			state->input_file_cur = -1;
			state->input_file_size = 0;
			printf("load_file from %s\n", param);
		}
	} else if (strncmp(cmd, PLUGIN_NAME ".start_recording", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			rtp_start_recording(state->rtp, param);
			printf("start_recording : completed\n");
		}
	} else if (strncmp(cmd, PLUGIN_NAME ".stop_recording", sizeof(buff)) == 0) {
		rtp_stop_recording(state->rtp);
		printf("stop_recording : completed\n");
	} else if (strncmp(cmd, PLUGIN_NAME ".start_loading", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			rtp_start_loading(state->rtp, param, true, true, (RTP_LOADING_CALLBACK) loading_callback, NULL);
			printf("start_loading : completed\n");
		}
	} else if (strncmp(cmd, PLUGIN_NAME ".stop_loading", sizeof(buff)) == 0) {
		rtp_stop_loading(state->rtp);
		printf("stop_loading : completed\n");
	} else if (strncmp(cmd, "cam_mode", sizeof(buff)) == 0) {
		state->input_mode = INPUT_MODE_CAM;
	} else if (strncmp(cmd, "get_loading_pos", sizeof(buff)) == 0) {
		if (state->input_file_size == 0) {
			printf("%d\n", -1);
		} else {
			double ratio = 100 * state->input_file_cur / state->input_file_size;
			printf("%d\n", (int) ratio);
		}
	} else if (strncmp(cmd, "set_camera_orientation", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			float pitch;
			float yaw;
			float roll;
			sscanf(param, "%f,%f,%f", &pitch, &yaw, &roll);
			state->camera_pitch = pitch * M_PI / 180.0;
			state->camera_yaw = yaw * M_PI / 180.0;
			state->camera_roll = roll * M_PI / 180.0;
			printf("set_camera_orientation\n");
		}
	} else if (strncmp(cmd, "set_view_quaternion", sizeof(buff)) == 0) {
		char *param = NULL;
		int id = 0; //default
		bool quat_valid = false;
		float x, y, z, w;
		bool fov_valid = false;
		float fov;
		bool client_key_valid = false;
		char *client_key = NULL;
		do {
			param = strtok(NULL, " \n");
			if (param != NULL) {
				if (strncmp(param, "quat=", 5) == 0) {
					int num = sscanf(param, "quat=%f,%f,%f,%f", &x, &y, &z, &w);
					if (num == 4) {
						quat_valid = true;
					}
				} else if (strncmp(param, "fov=", 4) == 0) {
					int num = sscanf(param, "fov=%f", &fov);
					if (num == 1) {
						fov_valid = true;
					}
				} else if (strncmp(param, "client_key=", 11) == 0) {
					client_key = param + 11;
					client_key_valid = true;
				} else if (strncmp(param, "id=", 3) == 0) {
					int num = sscanf(param, "id=%d", &id);
					if (num == 1) {
					}
				}
			}
		} while (param);

		if (quat_valid) {
			for (FRAME_T *frame = state->frame; frame != NULL; frame = frame->next) {
				if (frame->id == id && frame->view_mpu && frame->view_mpu->set_quaternion) {
					VECTOR4D_T value = { .ary = { x, y, z, w } };
					state->plugin_host.lock_texture();
					frame->view_mpu->set_quaternion(frame->view_mpu->user_data, value);
					state->plugin_host.unlock_texture();
					//printf("set_view_quaternion\n");
					if (fov_valid) {
						frame->fov = fov;
					}
					if (client_key_valid) {
						strncpy(frame->client_key, client_key, sizeof(frame->client_key));
						gettimeofday(&frame->server_key, NULL);
					}
					break;
				}
			}
		}
	} else if (strncmp(cmd, "set_fov", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			int id;
			float fov;
			sscanf(param, "%i=%f", &id, &fov);
			for (FRAME_T *frame = state->frame; frame != NULL; frame = frame->next) {
				if (frame->id == id) {
					frame->fov = fov;
					printf("set_fov\n");
					break;
				}
			}
		}
	} else if (strncmp(cmd, "set_stereo", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			state->stereo = (param[0] == '1');
			printf("set_stereo %s\n", param);
		}
	} else if (strncmp(cmd, "set_conf_sync", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			state->conf_sync = (param[0] == '1');
			printf("set_conf_sync %s\n", param);
		}
	} else if (strncmp(cmd, "set_preview", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			state->preview = (param[0] == '1');
			printf("set_preview %s\n", param);
		}
	} else if (strncmp(cmd, "add_camera_horizon_r", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			float *cam_horizon_r = (state->options.config_ex_enabled) ? state->options.cam_horizon_r_ex : state->options.cam_horizon_r;

			int cam_num = 0;
			float value = 0;
			if (param[0] == '*') {
				sscanf(param, "*=%f", &value);
				for (int i = 0; i < state->num_of_cam; i++) {
					cam_horizon_r[i] += value;
				}
			} else {
				sscanf(param, "%d=%f", &cam_num, &value);
				if (cam_num >= 0 && cam_num < state->num_of_cam) {
					cam_horizon_r[cam_num] += value;
				}
			}

			if (state->options.config_ex_enabled) { //try loading configuration
				save_options_ex(state);
			} else { //send upstream
				char cmd[256];
				sprintf(cmd, "upstream.add_camera_horizon_r %s", param);
				state->plugin_host.send_command(cmd);
			}

			printf("add_camera_horizon_r : completed\n");
		}
	} else if (strncmp(cmd, "add_camera_offset_x", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			float *cam_offset_x = (state->options.config_ex_enabled) ? state->options.cam_offset_x_ex : state->options.cam_offset_x;

			int cam_num = 0;
			float value = 0;
			if (param[0] == '*') {
				sscanf(param, "*=%f", &value);
				for (int i = 0; i < state->num_of_cam; i++) {
					cam_offset_x[i] += value;
				}
			} else {
				sscanf(param, "%d=%f", &cam_num, &value);
				if (cam_num >= 0 && cam_num < state->num_of_cam) {
					cam_offset_x[cam_num] += value;
				}
			}

			if (state->options.config_ex_enabled) { //try loading configuration
				save_options_ex(state);
			} else { //send upstream
				char cmd[256];
				sprintf(cmd, "upstream.add_camera_offset_x %s", param);
				state->plugin_host.send_command(cmd);
			}

			printf("add_camera_offset_x : completed\n");
		}
	} else if (strncmp(cmd, "add_camera_offset_y", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			float *cam_offset_y = (state->options.config_ex_enabled) ? state->options.cam_offset_y_ex : state->options.cam_offset_y;

			int cam_num = 0;
			float value = 0;
			if (param[0] == '*') {
				sscanf(param, "*=%f", &value);
				for (int i = 0; i < state->num_of_cam; i++) {
					cam_offset_y[i] += value;
				}
			} else {
				sscanf(param, "%d=%f", &cam_num, &value);
				if (cam_num >= 0 && cam_num < state->num_of_cam) {
					cam_offset_y[cam_num] += value;
				}
			}

			if (state->options.config_ex_enabled) { //try loading configuration
				save_options_ex(state);
			} else { //send upstream
				char cmd[256];
				sprintf(cmd, "upstream.add_camera_offset_y %s", param);
				state->plugin_host.send_command(cmd);
			}

			printf("add_camera_offset_y : completed\n");
		}
	} else if (strncmp(cmd, "add_camera_offset_yaw", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			int cam_num = 0;
			float value = 0;
			if (param[0] == '*') {
				sscanf(param, "*=%f", &value);
				for (int i = 0; i < state->num_of_cam; i++) {
					state->options.cam_offset_yaw[i] += value;
				}
			} else {
				sscanf(param, "%d=%f", &cam_num, &value);
				if (cam_num >= 0 && cam_num < state->num_of_cam) {
					state->options.cam_offset_yaw[cam_num] += value;
				}
			}
			{ //send upstream
				char cmd[256];
				sprintf(cmd, "upstream.add_camera_offset_yaw %s", param);
				state->plugin_host.send_command(cmd);
			}

			printf("add_camera_offset_yaw : completed\n");
		}
	} else if (strncmp(cmd, "set_camera_horizon_r_bias", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			float value = 0;
			sscanf(param, "%f", &value);
			state->camera_horizon_r_bias = value;
			printf("set_camera_horizon_r_bias : completed\n");
		}
	} else if (strncmp(cmd, "set_play_speed", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			float value = 0;
			sscanf(param, "%f", &value);
			state->rtp_play_speed = value;
			rtp_set_play_speed(state->rtp, value);
			printf("set_play_speed : completed\n");
		}
	} else if (strncmp(cmd, "add_color_offset", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			float value = 0;
			sscanf(param, "%f", &value);
			state->options.color_offset += value;
			printf("add_color_offset : completed\n");
		}
	} else if (strncmp(cmd, "set_frame_sync", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			state->frame_sync = (param[0] == '1');
			printf("set_frame_sync %s\n", param);
		}
	} else if (strncmp(cmd, "set_menu_visible", sizeof(buff)) == 0) {
		char *param = strtok(NULL, " \n");
		if (param != NULL) {
			state->menu_visible = (param[0] == '1');
			printf("set_menu_visible %s\n", param);
		}
	} else if (strncmp(cmd, "select_active_menu", sizeof(buff)) == 0) {
		menu_operate(state->menu, MENU_OPERATE_SELECT);
	} else if (strncmp(cmd, "deselect_active_menu", sizeof(buff)) == 0) {
		menu_operate(state->menu, MENU_OPERATE_DESELECT);
	} else if (strncmp(cmd, "go2next_menu", sizeof(buff)) == 0) {
		menu_operate(state->menu, MENU_OPERATE_ACTIVE_NEXT);
	} else if (strncmp(cmd, "back2previouse_menu", sizeof(buff)) == 0) {
		menu_operate(state->menu, MENU_OPERATE_ACTIVE_BACK);
	} else if (state->frame != NULL && strcasecmp(state->frame->renderer->name, "CALIBRATION") == 0) {
		if (strncmp(cmd, "step", sizeof(buff)) == 0) {
			char *param = strtok(NULL, " \n");
			if (param != NULL) {
				sscanf(param, "%lf", &calib_step);
			}
		}
		if (strncmp(cmd, "u", sizeof(buff)) == 0 || strncmp(cmd, "t", sizeof(buff)) == 0) {
			state->options.cam_offset_y[state->active_cam] += calib_step;
		}
		if (strncmp(cmd, "d", sizeof(buff)) == 0 || strncmp(cmd, "b", sizeof(buff)) == 0) {
			state->options.cam_offset_y[state->active_cam] -= calib_step;
		}
		if (strncmp(cmd, "l", sizeof(buff)) == 0) {
			state->options.cam_offset_x[state->active_cam] += calib_step;
		}
		if (strncmp(cmd, "r", sizeof(buff)) == 0) {
			state->options.cam_offset_x[state->active_cam] -= calib_step;
		}
		if (strncmp(cmd, "s", sizeof(buff)) == 0) {
			state->options.sharpness_gain += calib_step;
		}
		if (strncmp(cmd, "w", sizeof(buff)) == 0) {
			state->options.sharpness_gain -= calib_step;
		}
	} else {
		printf("unknown command : %s\n", buff);
	}
	return ret;
}

int command_handler() {
	int ret = 0;

	for (int i = 0; i < 10; i++) {
		char *buff = NULL;
		{
			pthread_mutex_lock(&state->cmd_list_mutex);

			if (state->cmd_list) {
				LIST_T *cur = state->cmd_list;
				buff = (char*) cur->value;
				state->cmd_list = cur->next;
				free(cur);
			}

			pthread_mutex_unlock(&state->cmd_list_mutex);
		}

		if (buff) {
			bool handled = false;
			for (int i = 0; state->plugins[i] != NULL; i++) {
				int name_len = strlen(state->plugins[i]->name);
				if (strncmp(buff, state->plugins[i]->name, name_len) == 0 && buff[name_len] == '.') {
					ret = state->plugins[i]->command_handler(state->plugins[i]->user_data, buff);
					handled = true;
				}
			}
			if (!handled) {
				ret = _command_handler(buff);
			}
			free(buff);
		} else {
			break;
		}
	}
	return ret;
}

static void *readline_thread_func(void* arg) {
	char *ptr;
	using_history();
	read_history(PICAM360_HISTORY_FILE);
	while ((ptr = readline("picam360-capture>")) != NULL) {
		add_history(ptr);
		state->plugin_host.send_command(ptr);
		free(ptr);

		write_history(PICAM360_HISTORY_FILE);
	}
	return NULL;
}

static void *quaternion_thread_func(void* arg) {
	int count = 0;
	while (1) {
		if (state->mpu) {
			int cur = (state->quaternion_queue_cur + 1) % MAX_QUATERNION_QUEUE_COUNT;
			state->quaternion_queue[cur] = state->mpu->get_quaternion(state->mpu);
			state->quaternion_queue_cur++;
		}
		usleep(QUATERNION_QUEUE_RES * 1000);
	}
	return NULL;
}
///////////////////////////////////////////
#if (1) //plugin host methods
static VECTOR4D_T get_view_quaternion() {
	VECTOR4D_T ret = { };
	if (state->frame && state->frame->view_mpu) {
		ret = state->frame->view_mpu->get_quaternion(state->frame->view_mpu);
	}
	return ret;
}
static VECTOR4D_T get_view_compass() {
	VECTOR4D_T ret = { };
	if (state->frame && state->frame->view_mpu) {
		ret = state->frame->view_mpu->get_compass(state->frame->view_mpu);
	}
	return ret;
}
static float get_view_temperature() {
	if (state->frame && state->frame->view_mpu) {
		return state->frame->view_mpu->get_temperature(state->frame->view_mpu);
	}
	return 0;
}
static float get_view_north() {
	if (state->frame && state->frame->view_mpu) {
		return state->frame->view_mpu->get_north(state->frame->view_mpu);
	}
	return 0;
}

static VECTOR4D_T get_camera_offset(int cam_num) {
	VECTOR4D_T ret = { };
	pthread_mutex_lock(&state->mutex);

	if (cam_num >= 0 && cam_num < state->num_of_cam) {
		ret.x = state->options.cam_offset_x[cam_num];
		ret.y = state->options.cam_offset_y[cam_num];
		ret.z = state->options.cam_offset_yaw[cam_num];
		ret.w = state->options.cam_horizon_r[cam_num];
	}

	pthread_mutex_unlock(&state->mutex);
	return ret;
}
static void set_camera_offset(int cam_num, VECTOR4D_T value) {
	if (!state->conf_sync) {
		return;
	}

	pthread_mutex_lock(&state->mutex);

	if (cam_num >= 0 && cam_num < state->num_of_cam) {
		state->options.cam_offset_x[cam_num] = value.x;
		state->options.cam_offset_y[cam_num] = value.y;
		state->options.cam_offset_yaw[cam_num] = value.z;
		state->options.cam_horizon_r[cam_num] = value.w;
	}

	pthread_mutex_unlock(&state->mutex);
}

static VECTOR4D_T get_camera_quaternion(int cam_num) {
	VECTOR4D_T ret = { };
	pthread_mutex_lock(&state->mutex);

	if (cam_num >= 0 && cam_num < state->num_of_cam) {
		ret = state->camera_quaternion[cam_num];
	} else {
		ret = state->camera_quaternion[MAX_CAM_NUM];
	}

	pthread_mutex_unlock(&state->mutex);
	return ret;
}
static void set_camera_quaternion(int cam_num, VECTOR4D_T value) {
	pthread_mutex_lock(&state->mutex);

	if (cam_num >= 0 && cam_num < state->num_of_cam) {
		state->camera_quaternion[cam_num] = value;
	}
	state->camera_quaternion[MAX_CAM_NUM] = value; //latest
	state->camera_coordinate_from_device = true;

	pthread_mutex_unlock(&state->mutex);
}
static VECTOR4D_T get_camera_compass() {
	VECTOR4D_T ret = { };
	pthread_mutex_lock(&state->mutex);

	ret = state->camera_compass;

	pthread_mutex_unlock(&state->mutex);
	return ret;
}
static void set_camera_compass(VECTOR4D_T value) {
	pthread_mutex_lock(&state->mutex);

	state->camera_compass = value;

	pthread_mutex_unlock(&state->mutex);
}
static float get_camera_temperature() {
	return state->camera_temperature;
}
static void set_camera_temperature(float value) {
	state->camera_temperature = value;
}
static float get_camera_north() {
	return state->camera_north;
}
static void set_camera_north(float value) {
	state->camera_north = value;
}
static void decode_video(int cam_num, unsigned char *data, int data_len) {
	if (state->decoders[cam_num]) {
		state->decoders[cam_num]->decode(state->decoders[cam_num], data, data_len);
	}
}
static void lock_texture() {
	pthread_mutex_lock(&state->texture_mutex);
}
static void unlock_texture() {
	pthread_mutex_unlock(&state->texture_mutex);
}
static void set_cam_texture_cur(int cam_num, int cur) {
	state->cam_texture_cur[cam_num] = cur;
}
static void get_texture_size(uint32_t *width_out, uint32_t *height_out) {
	if (width_out) {
		*width_out = state->cam_width;
	}
	if (height_out) {
		*height_out = state->cam_height;
	}
}
static void set_texture_size(uint32_t width, uint32_t height) {
	pthread_mutex_lock(&state->texture_size_mutex);
	if (state->cam_width == width && state->cam_height == height) {
		//do nothing
	} else {
		state->cam_width = width;
		state->cam_height = height;
	}
	pthread_mutex_unlock(&state->texture_size_mutex);
}
static MENU_T *get_menu() {
	return state->menu;
}
static bool get_menu_visible() {
	return state->menu_visible;
}
static void set_menu_visible(bool value) {
	state->menu_visible = value;
}

static float get_fov() {
	return state->frame->fov;
}
static void set_fov(float value) {
	state->frame->fov = value;
}

static MPU_T *get_mpu() {
	return state->mpu;
}
static RTP_T *get_rtp() {
	return state->rtp;
}
static RTP_T *get_rtcp() {
	return state->rtcp;
}

static int xmp(char *buff, int buff_len, int cam_num) {
	int xmp_len = 0;

	VECTOR4D_T quat = { };
	{
		int video_delay_ms = 0;
		int cur = (state->quaternion_queue_cur - video_delay_ms / QUATERNION_QUEUE_RES + MAX_QUATERNION_QUEUE_COUNT) % MAX_QUATERNION_QUEUE_COUNT;
		quat = state->quaternion_queue[cur];
	}
	VECTOR4D_T compass = state->mpu->get_compass(state->mpu);
	VECTOR4D_T camera_offset = state->plugin_host.get_camera_offset(cam_num);

	xmp_len = 0;
	buff[xmp_len++] = 0xFF;
	buff[xmp_len++] = 0xE1;
	buff[xmp_len++] = 0; // size MSB
	buff[xmp_len++] = 0; // size LSB
	xmp_len += sprintf(buff + xmp_len, "http://ns.adobe.com/xap/1.0/");
	buff[xmp_len++] = '\0';
	xmp_len += sprintf(buff + xmp_len, "<?xpacket begin=\"﻿");
	buff[xmp_len++] = 0xEF;
	buff[xmp_len++] = 0xBB;
	buff[xmp_len++] = 0xBF;
	xmp_len += sprintf(buff + xmp_len, "\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>");
	xmp_len += sprintf(buff + xmp_len, "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"picam360-drive rev1\">");
	xmp_len += sprintf(buff + xmp_len, "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">");
	xmp_len += sprintf(buff + xmp_len, "<rdf:Description rdf:about=\"\">");
	xmp_len += sprintf(buff + xmp_len, "<quaternion x=\"%f\" y=\"%f\" z=\"%f\" w=\"%f\" />", quat.x, quat.y, quat.z, quat.w);
	xmp_len += sprintf(buff + xmp_len, "<compass x=\"%f\" y=\"%f\" z=\"%f\" />", compass.x, compass.y, compass.z);
	xmp_len += sprintf(buff + xmp_len, "<temperature v=\"%f\" />", state->mpu->get_temperature(state->mpu));
	xmp_len += sprintf(buff + xmp_len, "<offset x=\"%f\" y=\"%f\" yaw=\"%f\" horizon_r=\"%f\" />", camera_offset.x, camera_offset.y, camera_offset.z, camera_offset.w);
	xmp_len += sprintf(buff + xmp_len, "</rdf:Description>");
	xmp_len += sprintf(buff + xmp_len, "</rdf:RDF>");
	xmp_len += sprintf(buff + xmp_len, "</x:xmpmeta>");
	xmp_len += sprintf(buff + xmp_len, "<?xpacket end=\"w\"?>");
	buff[xmp_len++] = '\0';
	buff[2] = ((xmp_len - 2) >> 8) & 0xFF; // size MSB
	buff[3] = (xmp_len - 2) & 0xFF; // size LSB

	return xmp_len;
}

static void send_command(const char *_cmd) {
	pthread_mutex_lock(&state->cmd_list_mutex);

	char *cmd = (char*) _cmd;
	LIST_T **cur = NULL;
	if (strncmp(cmd, ENDPOINT_DOMAIN, ENDPOINT_DOMAIN_SIZE) == 0) {
		if (state->options.rtp_rx_port == 0) {
			//endpoint
			cur = &state->cmd_list;
			cmd += ENDPOINT_DOMAIN_SIZE;
		} else {
			//send to upstream
			cur = &state->cmd2upstream_list;
		}
	} else if (strncmp(cmd, UPSTREAM_DOMAIN, UPSTREAM_DOMAIN_SIZE) == 0) {
		cur = &state->cmd2upstream_list;
		cmd += UPSTREAM_DOMAIN_SIZE;
	} else {
		cur = &state->cmd_list;
	}
	for (; *cur != NULL; cur = &(*cur)->next)
		;
	*cur = malloc(sizeof(LIST_T));
	memset(*cur, 0, sizeof(LIST_T));
	int slr_len = MIN(strlen(cmd), 256);
	char *cmd_clone = malloc(slr_len + 1);
	strncpy(cmd_clone, cmd, slr_len);
	cmd_clone[slr_len] = '\0';
	(*cur)->value = cmd_clone;

	pthread_mutex_unlock(&state->cmd_list_mutex);
}

static void event_handler(uint32_t node_id, uint32_t event_id) {
	switch (node_id) {
	case PICAM360_HOST_NODE_ID:
		switch (event_id) {
		case PICAM360_CAPTURE_EVENT_AFTER_SNAP:
			convert_snap_handler();
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

static void send_event(uint32_t node_id, uint32_t event_id) {
	event_handler(node_id, event_id);
	for (int i = 0; state->plugins && state->plugins[i] != NULL; i++) {
		if (state->plugins[i]) {
			state->plugins[i]->event_handler(state->plugins[i]->user_data, node_id, event_id);
		}
	}
}

static void add_mpu_factory(MPU_FACTORY_T *mpu_factory) {
	for (int i = 0; state->mpu_factories[i] != (void*) -1; i++) {
		if (state->mpu_factories[i] == NULL) {
			state->mpu_factories[i] = mpu_factory;
			if (state->mpu_factories[i + 1] == (void*) -1) {
				int space = (i + 2) * 2;
				if (space > 256) {
					fprintf(stderr, "error on add_mpu_factory\n");
					return;
				}
				MPU_FACTORY_T **current = state->mpu_factories;
				state->mpu_factories = malloc(sizeof(MPU_FACTORY_T*) * space);
				memset(state->mpu_factories, 0, sizeof(MPU_FACTORY_T*) * space);
				memcpy(state->mpu_factories, current, sizeof(MPU_FACTORY_T*) * i);
				state->mpu_factories[space - 1] = (void*) -1;
				free(current);
			}
			return;
		}
	}
}

static void add_capture_factory(CAPTURE_FACTORY_T *capture_factory) {
	for (int i = 0; state->capture_factories[i] != (void*) -1; i++) {
		if (state->capture_factories[i] == NULL) {
			state->capture_factories[i] = capture_factory;
			if (state->capture_factories[i + 1] == (void*) -1) {
				int space = (i + 2) * 2;
				if (space > 256) {
					fprintf(stderr, "error on add_capture_factory\n");
					return;
				}
				CAPTURE_FACTORY_T **current = state->capture_factories;
				state->capture_factories = malloc(sizeof(CAPTURE_FACTORY_T*) * space);
				memset(state->capture_factories, 0, sizeof(CAPTURE_FACTORY_T*) * space);
				memcpy(state->capture_factories, current, sizeof(CAPTURE_FACTORY_T*) * i);
				state->capture_factories[space - 1] = (void*) -1;
				free(current);
			}
			return;
		}
	}
}

static void add_decoder_factory(DECODER_FACTORY_T *decoder_factory) {
	for (int i = 0; state->decoder_factories[i] != (void*) -1; i++) {
		if (state->decoder_factories[i] == NULL) {
			state->decoder_factories[i] = decoder_factory;
			if (state->decoder_factories[i + 1] == (void*) -1) {
				int space = (i + 2) * 2;
				if (space > 256) {
					fprintf(stderr, "error on add_decoder_factory\n");
					return;
				}
				DECODER_FACTORY_T **current = state->decoder_factories;
				state->decoder_factories = malloc(sizeof(DECODER_FACTORY_T*) * space);
				memset(state->decoder_factories, 0, sizeof(DECODER_FACTORY_T*) * space);
				memcpy(state->decoder_factories, current, sizeof(DECODER_FACTORY_T*) * i);
				state->decoder_factories[space - 1] = (void*) -1;
				free(current);
			}
			return;
		}
	}
}

static void add_encoder_factory(ENCODER_FACTORY_T *encoder_factory) {
	for (int i = 0; state->encoder_factories[i] != (void*) -1; i++) {
		if (state->encoder_factories[i] == NULL) {
			state->encoder_factories[i] = encoder_factory;
			if (state->encoder_factories[i + 1] == (void*) -1) {
				int space = (i + 2) * 2;
				if (space > 256) {
					fprintf(stderr, "error on add_encoder_factory\n");
					return;
				}
				ENCODER_FACTORY_T **current = state->encoder_factories;
				state->encoder_factories = malloc(sizeof(ENCODER_FACTORY_T*) * space);
				memset(state->encoder_factories, 0, sizeof(ENCODER_FACTORY_T*) * space);
				memcpy(state->encoder_factories, current, sizeof(ENCODER_FACTORY_T*) * i);
				state->encoder_factories[space - 1] = (void*) -1;
				free(current);
			}
			return;
		}
	}
}

static void add_renderer(RENDERER_T *renderer) {
#ifdef USE_GLES
	char *common = "#version 100\n";
#else
	char *common = "#version 330\n";
#endif
	renderer->init(renderer, common, state->num_of_cam);

	for (int i = 0; state->renderers[i] != (void*) -1; i++) {
		if (state->renderers[i] == NULL) {
			state->renderers[i] = renderer;
			if (state->renderers[i + 1] == (void*) -1) {
				int space = (i + 2) * 2;
				if (space > 256) {
					fprintf(stderr, "error on add_renderer\n");
					return;
				}
				RENDERER_T **current = state->renderers;
				state->renderers = malloc(sizeof(RENDERER_T*) * space);
				memset(state->renderers, 0, sizeof(RENDERER_T*) * space);
				memcpy(state->renderers, current, sizeof(RENDERER_T*) * i);
				state->renderers[space - 1] = (void*) -1;
				free(current);
			}
			return;
		}
	}
}

static void add_status(STATUS_T *status) {
	for (int i = 0; state->statuses[i] != (void*) -1; i++) {
		if (state->statuses[i] == NULL) {
			state->statuses[i] = status;
			if (state->statuses[i + 1] == (void*) -1) {
				int space = (i + 2) * 2;
				if (space > 256) {
					fprintf(stderr, "error on add_status\n");
					return;
				}
				STATUS_T **current = state->statuses;
				state->statuses = malloc(sizeof(STATUS_T*) * space);
				memset(state->statuses, 0, sizeof(STATUS_T*) * space);
				memcpy(state->statuses, current, sizeof(STATUS_T*) * i);
				state->statuses[space - 1] = (void*) -1;
				free(current);
			}
			return;
		}
	}
}

static void add_watch(STATUS_T *watch) {
	for (int i = 0; state->watches[i] != (void*) -1; i++) {
		if (state->watches[i] == NULL) {
			state->watches[i] = watch;
			if (state->watches[i + 1] == (void*) -1) {
				int space = (i + 2) * 2;
				if (space > 256) {
					fprintf(stderr, "error on add_watch\n");
					return;
				}
				STATUS_T **current = state->watches;
				state->watches = malloc(sizeof(STATUS_T*) * space);
				memset(state->watches, 0, sizeof(STATUS_T*) * space);
				memcpy(state->watches, current, sizeof(STATUS_T*) * i);
				state->watches[space - 1] = (void*) -1;
				free(current);
			}
			return;
		}
	}
}

static void add_plugin(PLUGIN_T *plugin) {
	for (int i = 0; state->plugins[i] != (void*) -1; i++) {
		if (state->plugins[i] == NULL) {
			state->plugins[i] = plugin;
			if (state->plugins[i + 1] == (void*) -1) {
				int space = (i + 2) * 2;
				if (space > 256) {
					fprintf(stderr, "error on add_plugin\n");
					return;
				}
				PLUGIN_T **current = state->plugins;
				state->plugins = malloc(sizeof(PLUGIN_T*) * space);
				memset(state->plugins, 0, sizeof(PLUGIN_T*) * space);
				memcpy(state->plugins, current, sizeof(PLUGIN_T*) * i);
				state->plugins[space - 1] = (void*) -1;
				free(current);
			}
			return;
		}
	}
}

static void snap(uint32_t width, uint32_t height, enum RENDERING_MODE mode, const char *path) {
	char cmd[512];
	char *mode_str = "";
	switch (mode) {
	case RENDERING_MODE_EQUIRECTANGULAR:
		mode_str = "-E";
		break;
	default:
		break;
	}

	sprintf(cmd, "snap -w %d -h %d %s -o %s", width, height, mode_str, path);
	state->plugin_host.send_command(cmd);
}

static void init_plugins(PICAM360CAPTURE_T *state) {
	{ //init host
		state->plugin_host.get_view_quaternion = get_view_quaternion;
		state->plugin_host.get_view_compass = get_view_compass;
		state->plugin_host.get_view_temperature = get_view_temperature;
		state->plugin_host.get_view_north = get_view_north;

		state->plugin_host.get_camera_offset = get_camera_offset;
		state->plugin_host.set_camera_offset = set_camera_offset;
		state->plugin_host.get_camera_quaternion = get_camera_quaternion;
		state->plugin_host.set_camera_quaternion = set_camera_quaternion;
		state->plugin_host.get_camera_compass = get_camera_compass;
		state->plugin_host.set_camera_compass = set_camera_compass;
		state->plugin_host.get_camera_temperature = get_camera_temperature;
		state->plugin_host.set_camera_temperature = set_camera_temperature;
		state->plugin_host.get_camera_north = get_camera_north;
		state->plugin_host.set_camera_north = set_camera_north;

		state->plugin_host.decode_video = decode_video;
		state->plugin_host.lock_texture = lock_texture;
		state->plugin_host.unlock_texture = unlock_texture;
		state->plugin_host.set_cam_texture_cur = set_cam_texture_cur;
		state->plugin_host.get_texture_size = get_texture_size;
		state->plugin_host.set_texture_size = set_texture_size;
		state->plugin_host.load_texture = load_texture;

		state->plugin_host.get_menu = get_menu;
		state->plugin_host.get_menu_visible = get_menu_visible;
		state->plugin_host.set_menu_visible = set_menu_visible;

		state->plugin_host.get_fov = get_fov;
		state->plugin_host.set_fov = set_fov;

		state->plugin_host.get_mpu = get_mpu;
		state->plugin_host.get_rtp = get_rtp;
		state->plugin_host.get_rtcp = get_rtcp;
		state->plugin_host.xmp = xmp;

		state->plugin_host.send_command = send_command;
		state->plugin_host.send_event = send_event;
		state->plugin_host.add_mpu_factory = add_mpu_factory;
		state->plugin_host.add_capture_factory = add_capture_factory;
		state->plugin_host.add_decoder_factory = add_decoder_factory;
		state->plugin_host.add_encoder_factory = add_encoder_factory;
		state->plugin_host.add_renderer = add_renderer;
		state->plugin_host.add_status = add_status;
		state->plugin_host.add_watch = add_watch;
		state->plugin_host.add_plugin = add_plugin;

		state->plugin_host.snap = snap;
	}

	{
		MPU_FACTORY_T *mpu_factory = NULL;
		create_manual_mpu_factory(&mpu_factory);
		state->plugin_host.add_mpu_factory(mpu_factory);
	}

	//load plugins
	json_error_t error;
	json_t *options = json_load_file_without_comment(state->config_filepath, 0, &error);
	if (options == NULL) {
		fputs(error.text, stderr);
	} else {
		{
			json_t *plugin_paths = json_object_get(options, "plugin_paths");
			if (json_is_array(plugin_paths)) {
				int size = json_array_size(plugin_paths);
				state->plugin_paths = (char**) malloc(sizeof(char*) * (size + 1));
				memset(state->plugin_paths, 0, sizeof(char*) * (size + 1));

				for (int i = 0; i < size; i++) {
					json_t *value = json_array_get(plugin_paths, i);
					int len = json_string_length(value);
					state->plugin_paths[i] = (char*) malloc(sizeof(char) * (len + 1));
					memset(state->plugin_paths[i], 0, sizeof(char) * (len + 1));
					strncpy(state->plugin_paths[i], json_string_value(value), len);
					if (len > 0) {
						void *handle = dlopen(state->plugin_paths[i], RTLD_LAZY);
						if (!handle) {
							fprintf(stderr, "%s\n", dlerror());
							continue;
						}
						CREATE_PLUGIN create_plugin = (CREATE_PLUGIN) dlsym(handle, "create_plugin");
						if (!create_plugin) {
							fprintf(stderr, "%s\n", dlerror());
							dlclose(handle);
							continue;
						}
						PLUGIN_T *plugin = NULL;
						create_plugin(&state->plugin_host, &plugin);
						if (!plugin) {
							fprintf(stderr, "%s\n", "create_plugin fail.");
							dlclose(handle);
							continue;
						}
						printf("plugin %s loaded.\n", state->plugin_paths[i]);
						state->plugin_host.add_plugin(plugin);
					}
				}
			}
		}

		for (int i = 0; state->plugins[i] != NULL; i++) {
			if (state->plugins[i]->init_options) {
				state->plugins[i]->init_options(state->plugins[i]->user_data, options);
			}
		}

		json_decref(options);
	}
}

#endif //plugin block

///////////////////////////////////////////////////////
#if (1) //rtp block

#define PT_STATUS 100
#define PT_CMD 101
#define PT_CAM_BASE 110

static char lg_command[256] = { };
static int lg_command_id = 0;
static int lg_ack_command_id_downstream = -1;
static int lg_ack_command_id_upstream = -1;

static bool lg_debug_dump = false;
static int lg_debug_dump_num = 0;
static int lg_debug_dump_fd = -1;
static void stream_callback(unsigned char *data, unsigned int data_len, void *frame_data, void *user_data) {
	FRAME_T *frame = (FRAME_T*) user_data;
	FRAME_INFO_T *frame_info = (FRAME_INFO_T*) frame_data;
	if (frame->output_mode == OUTPUT_MODE_STREAM) {
		if (frame->output_type == OUTPUT_TYPE_H265) {
			const unsigned char SC[] = { 0x00, 0x00, 0x00, 0x01 };
			const unsigned char SOI[] = { 0x48, 0x45 }; //'H', 'E'
			const unsigned char EOI[] = { 0x56, 0x43 }; //'V', 'C'
			//header pack
			//printf("debug info h265 len=%d, type=%d\n", data_len, ((data[4] & 0x7e) >> 1));
			if (frame_info) { // sei for a frame
				int server_key = frame_info->server_key.tv_sec * 1000 + frame_info->server_key.tv_usec;
				float idle_time_sec = 0;
				float frame_processed_sec = 0;
				float encoded_sec = 0;
				if (frame_info->client_key[0] != '\0') {
					struct timeval diff;
					gettimeofday(&frame_info->after_encoded, NULL);

					timersub(&frame_info->before_redraw_render_texture, &frame_info->server_key, &diff);
					idle_time_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;

					timersub(&frame_info->after_redraw_render_texture, &frame_info->before_redraw_render_texture, &diff);
					frame_processed_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;

					timersub(&frame_info->after_encoded, &frame_info->after_redraw_render_texture, &diff);
					encoded_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;
				}

				unsigned char header_pack[512];
				char *sei = (char*) header_pack + sizeof(SOI);
				sei[4] = 40 << 1; //nal_type:sei
				int len =
						sprintf(sei + 5,
								"<picam360:frame frame_id=\"%d\" mode=\"%s\" view_quat=\"%.3f,%.3f,%.3f,%.3f\" fov=\"%.3f\" client_key=\"%s\" server_key=\"%d\" idle_time=\"%.3f\" frame_processed=\"%.3f\" encoded=\"%.3f\" />",
								frame->id, frame->renderer->name, frame_info->view_quat.x, frame_info->view_quat.y, frame_info->view_quat.z, frame_info->view_quat.w, frame_info->fov,
								frame_info->client_key, server_key, idle_time_sec, frame_processed_sec, encoded_sec);
				len += 1; //nal header
				sei[0] = (len >> 24) & 0xFF;
				sei[1] = (len >> 16) & 0xFF;
				sei[2] = (len >> 8) & 0xFF;
				sei[3] = (len >> 0) & 0xFF;
				len += 4; //start code
				memcpy(header_pack, SOI, sizeof(SOI));
				len += 2; //SOI
				rtp_sendpacket(state->rtp, header_pack, len, PT_CAM_BASE);
			} else {
				unsigned char header_pack[512];
				char *sei = (char*) header_pack + sizeof(SOI);
				sei[4] = 40 << 1; //nal_type:sei
				int len = sprintf(sei + 5, "<picam360:frame frame_id=\"%d\" />", frame->id);
				len += 1; //nal header
				sei[0] = (len >> 24) & 0xFF;
				sei[1] = (len >> 16) & 0xFF;
				sei[2] = (len >> 8) & 0xFF;
				sei[3] = (len >> 0) & 0xFF;
				len += 4; //start code
				memcpy(header_pack, SOI, sizeof(SOI));
				len += 2; //SOI
				rtp_sendpacket(state->rtp, header_pack, len, PT_CAM_BASE);
			}
			if (frame->output_fd > 0) {
				if (!frame->output_start) {
					if (((data[4] & 0x7e) >> 1) == 32) { // wait for vps
						printf("output_start\n");
						frame->output_start = true;
					}
				}
				if (frame->output_start) {
					write(frame->output_fd, SC, 4);
					write(frame->output_fd, data + 4, data_len - 4);
				}
			}
			for (int j = 0; j < data_len;) {
				int len;
				if (j + RTP_MAXPAYLOADSIZE < data_len) {
					len = RTP_MAXPAYLOADSIZE;
				} else {
					len = data_len - j;
				}
				rtp_sendpacket(state->rtp, data + j, len, PT_CAM_BASE);
				j += len;
			}
			rtp_sendpacket(state->rtp, EOI, sizeof(EOI), PT_CAM_BASE);
			rtp_flush(state->rtp);
		} else if (frame->output_type == OUTPUT_TYPE_H264) {
			const unsigned char SC[] = { 0x00, 0x00, 0x00, 0x01 };
			const unsigned char SOI[] = { 0x4E, 0x41 }; //'N', 'A'
			const unsigned char EOI[] = { 0x4C, 0x55 }; //'L', 'U'
			//header pack
			if (frame_info) { // sei for a frame
				int server_key = frame_info->server_key.tv_sec * 1000 + frame_info->server_key.tv_usec;
				float idle_time_sec = 0;
				float frame_processed_sec = 0;
				float encoded_sec = 0;
				if (frame_info->client_key[0] != '\0') {
					struct timeval diff;
					gettimeofday(&frame_info->after_encoded, NULL);

					timersub(&frame_info->before_redraw_render_texture, &frame_info->server_key, &diff);
					idle_time_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;

					timersub(&frame_info->after_redraw_render_texture, &frame_info->before_redraw_render_texture, &diff);
					frame_processed_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;

					timersub(&frame_info->after_encoded, &frame_info->after_redraw_render_texture, &diff);
					encoded_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;
				}

				unsigned char header_pack[512];
				char *sei = (char*) header_pack + sizeof(SOI);
				sei[4] = 6; //nal_type:sei
				int len =
						sprintf(sei + 5,
								"<picam360:frame frame_id=\"%d\" mode=\"%s\" view_quat=\"%.3f,%.3f,%.3f,%.3f\" fov=\"%.3f\" client_key=\"%s\" server_key=\"%d\" idle_time=\"%.3f\" frame_processed=\"%.3f\" encoded=\"%.3f\" />",
								frame->id, frame->renderer->name, frame_info->view_quat.x, frame_info->view_quat.y, frame_info->view_quat.z, frame_info->view_quat.w, frame_info->fov,
								frame_info->client_key, server_key, idle_time_sec, frame_processed_sec, encoded_sec);
				len += 1; //nal header
				sei[0] = (len >> 24) & 0xFF;
				sei[1] = (len >> 16) & 0xFF;
				sei[2] = (len >> 8) & 0xFF;
				sei[3] = (len >> 0) & 0xFF;
				len += 4; //start code
				memcpy(header_pack, SOI, sizeof(SOI));
				len += 2; //SOI
				rtp_sendpacket(state->rtp, header_pack, len, PT_CAM_BASE);
			} else {
				unsigned char header_pack[512];
				char *sei = (char*) header_pack + sizeof(SOI);
				sei[4] = 6; //nal_type:sei
				int len = sprintf(sei + 5, "<picam360:frame frame_id=\"%d\" />", frame->id);
				len += 1; //nal header
				sei[0] = (len >> 24) & 0xFF;
				sei[1] = (len >> 16) & 0xFF;
				sei[2] = (len >> 8) & 0xFF;
				sei[3] = (len >> 0) & 0xFF;
				len += 4; //start code
				memcpy(header_pack, SOI, sizeof(SOI));
				len += 2; //SOI
				rtp_sendpacket(state->rtp, header_pack, len, PT_CAM_BASE);
			}
			if (frame->output_fd > 0) {
				if (!frame->output_start) {
					if ((data[4] & 0x1f) == 7) { // wait for sps
						printf("output_start\n");
						frame->output_start = true;
					}
				}
				if (frame->output_start) {
					write(frame->output_fd, SC, 4);
					write(frame->output_fd, data + 4, data_len - 4);
				}
			}
			for (int j = 0; j < data_len;) {
				int len;
				if (j + RTP_MAXPAYLOADSIZE < data_len) {
					len = RTP_MAXPAYLOADSIZE;
				} else {
					len = data_len - j;
				}
				rtp_sendpacket(state->rtp, data + j, len, PT_CAM_BASE);
				j += len;
			}
			rtp_sendpacket(state->rtp, EOI, sizeof(EOI), PT_CAM_BASE);
			rtp_flush(state->rtp);
		} else if (frame->output_type == OUTPUT_TYPE_MJPEG) {
			if (lg_debug_dump) {
				if (data[0] == 0xFF && data[1] == 0xD8) { //
					char path[256];
					sprintf(path, "/tmp/debug_%03d.jpeg", lg_debug_dump_num++);
					lg_debug_dump_fd = open(path, //
							O_CREAT | O_WRONLY | O_TRUNC, /*  */
							S_IRUSR | S_IWUSR | /* rw */
							S_IRGRP | S_IWGRP | /* rw */
							S_IROTH | S_IXOTH);
				}
				if (lg_debug_dump_fd > 0) {
					write(lg_debug_dump_fd, data, data_len);
				}
				if (data[data_len - 2] == 0xFF && data[data_len - 1] == 0xD9) {
					if (lg_debug_dump_fd > 0) {
						close(lg_debug_dump_fd);
						lg_debug_dump_fd = -1;
					}
				}
			}
			for (int i = 0; i < data_len;) {
				int len;
				if (i + RTP_MAXPAYLOADSIZE < data_len) {
					len = RTP_MAXPAYLOADSIZE;
				} else {
					len = data_len - i;
				}
				rtp_sendpacket(state->rtp, data + i, len, PT_CAM_BASE);
				i += len;
			}
			rtp_flush(state->rtp);
			if (frame->output_fd > 0) {
				write(frame->output_fd, data, data_len);
			}
		}
	}
	if (frame_info) {
		free(frame_info);
	}
}

static int command2upstream_handler() {
	static struct timeval last_try = { };
	static bool is_first_try = false;
	int len = strlen(lg_command);
	if (len != 0 && lg_command_id != lg_ack_command_id_upstream) {
		struct timeval s;
		gettimeofday(&s, NULL);
		if (!is_first_try) {
			struct timeval diff;
			timersub(&s, &last_try, &diff);
			float diff_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;
			if (diff_sec < 0.050) {
				return 0;
			}
		}
		rtp_sendpacket(state->rtcp, (unsigned char*) lg_command, len, PT_CMD);
		rtp_flush(state->rtcp);
		last_try = s;
		is_first_try = false;
		return 0;
	} else {
		memset(lg_command, 0, sizeof(lg_command));
	}

	char *buff = NULL;
	{
		pthread_mutex_lock(&state->cmd_list_mutex);

		if (state->cmd2upstream_list) {
			LIST_T *cur = state->cmd2upstream_list;
			buff = (char*) cur->value;
			state->cmd2upstream_list = cur->next;
			free(cur);
		}

		pthread_mutex_unlock(&state->cmd_list_mutex);
	}

	if (buff) {
		lg_command_id++;
		snprintf(lg_command, sizeof(lg_command), "<picam360:command id=\"%d\" value=\"%s\" />", lg_command_id, buff);
		free(buff);
	}
	return 0;
}

static void status_handler(char *data, int data_len) {
	for (int i = 0; i < data_len; i++) {
		if (data[i] == '<') {
			char name[64] = UPSTREAM_DOMAIN;
			char value[256];
			int num = sscanf(&data[i], "<picam360:status name=\"%63[^\"]\" value=\"%255[^\"]\" />", name + UPSTREAM_DOMAIN_SIZE, value);
			if (num == 2) {
				for (int i = 0; state->watches[i] != NULL; i++) {
					if (strncmp(state->watches[i]->name, name, 64) == 0) {
						state->watches[i]->set_value(state->watches[i]->user_data, value);
						break;
					}
				}
			}
		}
	}
}

static int rtp_callback(unsigned char *data, unsigned int data_len, unsigned char pt, unsigned int seq_num, void *user_data) {
	if (data_len == 0) {
		return -1;
	}
	static unsigned int last_seq_num = 0;
	if (seq_num != last_seq_num + 1) {
		printf("rtp : packet lost : from %d to %d\n", last_seq_num, seq_num);
	}
	last_seq_num = seq_num;

	if (pt == PT_STATUS) {
		status_handler((char*) data, data_len);
	}
	return 0;
}

static int rtcp_callback(unsigned char *data, unsigned int data_len, unsigned char pt, unsigned int seq_num) {
	if (data_len == 0) {
		return -1;
	}
	static unsigned int last_seq_num = -1;
	if (seq_num != last_seq_num + 1) {
		printf("rtcp : packet lost : from %d to %d\n", last_seq_num, seq_num);
	}
	last_seq_num = seq_num;

	if (pt == PT_CMD) {
		int id;
		char value[256];
		int num = sscanf((char*) data, "<picam360:command id=\"%d\" value=\"%255[^\"]\" />", &id, value);
		if (num == 2 && id != lg_ack_command_id_downstream) {
			lg_ack_command_id_downstream = id;
			state->plugin_host.send_command(value);
		}
	}
	return 0;
}

#if (1) //status block

#define STATUS_VAR(name) lg_status_ ## name
#define STATUS_INIT(plugin_host, prefix, name) STATUS_VAR(name) = new_status(prefix #name); \
                                               (plugin_host)->add_status(STATUS_VAR(name));
#define WATCH_VAR(name) lg_watch_ ## name
#define WATCH_INIT(plugin_host, prefix, name) WATCH_VAR(name) = new_status(prefix #name); \
                                               (plugin_host)->add_watch(WATCH_VAR(name));
//status to downstream
static STATUS_T *STATUS_VAR(ack_command_id);
static STATUS_T *STATUS_VAR(next_frame_id);
static STATUS_T *STATUS_VAR(quaternion);
static STATUS_T *STATUS_VAR(north);
static STATUS_T *STATUS_VAR(info);
static STATUS_T *STATUS_VAR(menu);
//watch status from upstream
static STATUS_T *WATCH_VAR(ack_command_id);
static STATUS_T *WATCH_VAR(quaternion);
static STATUS_T *WATCH_VAR(compass);
static STATUS_T *WATCH_VAR(temperature);
static STATUS_T *WATCH_VAR(bandwidth);
static STATUS_T *WATCH_VAR(cam_fps);
static STATUS_T *WATCH_VAR(cam_frameskip);

static void status_release(void *user_data) {
	free(user_data);
}
static void status_get_value(void *user_data, char *buff, int buff_len) {
	STATUS_T *status = (STATUS_T*) user_data;
	if (status == STATUS_VAR(ack_command_id)) {
		snprintf(buff, buff_len, "%d", lg_ack_command_id_downstream);
	} else if (status == STATUS_VAR(next_frame_id)) {
		snprintf(buff, buff_len, "%d", state->next_frame_id);
	} else if (status == STATUS_VAR(quaternion)) {
		VECTOR4D_T quat = state->mpu->get_quaternion(state->mpu);
		snprintf(buff, buff_len, "%f,%f,%f,%f", quat.x, quat.y, quat.z, quat.w);
	} else if (status == STATUS_VAR(north)) {
		float north = state->mpu->get_north(state->mpu);
		snprintf(buff, buff_len, "%f", north);
	} else if (status == STATUS_VAR(info)) {
		get_info_str(buff, buff_len);
	} else if (status == STATUS_VAR(menu)) {
		get_menu_str(buff, buff_len);
	}
}
static void status_set_value(void *user_data, const char *value) {
	STATUS_T *status = (STATUS_T*) user_data;
	if (status == WATCH_VAR(ack_command_id)) {
		sscanf(value, "%d", &lg_ack_command_id_upstream);
	} else if (status == WATCH_VAR(quaternion)) {
		VECTOR4D_T vec = { };
		sscanf(value, "%f,%f,%f,%f", &vec.x, &vec.y, &vec.z, &vec.w);
		state->plugin_host.set_camera_quaternion(-1, vec);
	} else if (status == WATCH_VAR(compass)) {
		VECTOR4D_T vec = { };
		sscanf(value, "%f,%f,%f,%f", &vec.x, &vec.y, &vec.z, &vec.w);
		state->plugin_host.set_camera_compass(vec);
	} else if (status == WATCH_VAR(temperature)) {
		float temperature = 0;
		sscanf(value, "%f", &temperature);
		state->plugin_host.set_camera_temperature(temperature);
	} else if (status == WATCH_VAR(bandwidth)) {
		sscanf(value, "%f", &lg_cam_bandwidth);
	} else if (status == WATCH_VAR(cam_fps)) {
		sscanf(value, "%f,%f", &lg_cam_fps[0], &lg_cam_fps[1]);
	} else if (status == WATCH_VAR(cam_frameskip)) {
		sscanf(value, "%f,%f", &lg_cam_frameskip[0], &lg_cam_frameskip[1]);
	}
}

static STATUS_T *new_status(const char *name) {
	STATUS_T *status = (STATUS_T*) malloc(sizeof(STATUS_T));
	strcpy(status->name, name);
	status->get_value = status_get_value;
	status->set_value = status_set_value;
	status->release = status_release;
	status->user_data = status;
	return status;
}

static void init_status() {
	STATUS_INIT(&state->plugin_host, "", ack_command_id);
	STATUS_INIT(&state->plugin_host, "", next_frame_id);
	STATUS_INIT(&state->plugin_host, "", quaternion);
	STATUS_INIT(&state->plugin_host, "", north);
	STATUS_INIT(&state->plugin_host, "", info);
	STATUS_INIT(&state->plugin_host, "", menu);
	WATCH_INIT(&state->plugin_host, UPSTREAM_DOMAIN, ack_command_id);
	WATCH_INIT(&state->plugin_host, UPSTREAM_DOMAIN, quaternion);
	WATCH_INIT(&state->plugin_host, UPSTREAM_DOMAIN, compass);
	WATCH_INIT(&state->plugin_host, UPSTREAM_DOMAIN, temperature);
	WATCH_INIT(&state->plugin_host, UPSTREAM_DOMAIN, bandwidth);
	WATCH_INIT(&state->plugin_host, UPSTREAM_DOMAIN, cam_fps);
	WATCH_INIT(&state->plugin_host, UPSTREAM_DOMAIN, cam_frameskip);
}

#endif //status block

static void _init_rtp(PICAM360CAPTURE_T *state) {
	state->rtp = create_rtp(state->options.rtp_rx_port, state->options.rtp_rx_type, state->options.rtp_tx_ip, state->options.rtp_tx_port, state->options.rtp_tx_type, 0);
	rtp_add_callback(state->rtp, (RTP_CALLBACK) rtp_callback, NULL);

	state->rtcp = create_rtp(state->options.rtcp_rx_port, state->options.rtcp_rx_type, state->options.rtcp_tx_ip, state->options.rtcp_tx_port, state->options.rtcp_tx_type, 0);
	rtp_add_callback(state->rtcp, (RTP_CALLBACK) rtcp_callback, NULL);

	init_status();
}

#endif //rtp block

///////////////////////////////////////////////////////
#if (1) //menu block

enum SYSTEM_CMD {
	SYSTEM_CMD_NONE, SYSTEM_CMD_SHUTDOWN, SYSTEM_CMD_REBOOT, SYSTEM_CMD_EXIT,
};

enum CALIBRATION_CMD {
	CALIBRATION_CMD_NONE, CALIBRATION_CMD_SAVE, CALIBRATION_CMD_IMAGE_CIRCLE, CALIBRATION_CMD_IMAGE_PARAMS, CALIBRATION_CMD_VIEWER_COMPASS, CALIBRATION_CMD_VEHICLE_COMPASS,
};

#define PACKET_FOLDER_PATH "/media/usbdisk/packet"
#define STILL_FOLDER_PATH "/media/usbdisk/still"
#define VIDEO_FOLDER_PATH "/media/usbdisk/video"

static int lg_resolution = 4;
static bool lg_stereo_enabled = false;
static bool lg_sync_enabled = true;

static char lg_convert_base_path[256];
static uint32_t lg_convert_frame_num = 0;
static bool lg_is_converting = false;

static void menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {

}

static int get_last_id(const char *path) {
	struct dirent *d;
	DIR *dir;

	dir = opendir(path);
	if (dir != NULL) {
		int last_id = 0;
		while ((d = readdir(dir)) != 0) {
			if (d->d_name[0] != L'.') {
				int id = 0;
				sscanf(d->d_name, "%d", &id);
				if (id > last_id) {
					last_id = id;
				}
			}
		}
		closedir(dir);
		return last_id;
	} else {
		return -1;
	}
}

static void loading_callback(void *user_data, int ret) {
	lg_is_converting = false;
	printf("end of loading\n");
}

static void convert_snap_handler() {
	if (lg_is_converting) {
		rtp_increment_loading(state->rtp, 100 * 1000); //10 fps

		char dst[256];
		snprintf(dst, 256, "%s/%d.jpeg", lg_convert_base_path, lg_convert_frame_num);
		lg_convert_frame_num++;
		state->plugin_host.snap(lg_resolution * 1024, lg_resolution * 512, RENDERING_MODE_EQUIRECTANGULAR, dst);
	}
}

static void packet_menu_record_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_ACTIVATED:
		break;
	case MENU_EVENT_DEACTIVATED:
		break;
	case MENU_EVENT_SELECTED:
		menu->selected = false;
		if (lg_is_converting) { //stop convert
			lg_is_converting = false;

			rtp_set_auto_play(state->rtp, true);
			rtp_set_is_looping(state->rtp, true);

			snprintf(menu->name, 8, "Record");
			printf("stop converting\n");
			menu->selected = false;
		} else if (rtp_is_loading(state->rtp, NULL)) { //start convert
			int ret = mkdir(lg_convert_base_path, //
					S_IRUSR | S_IWUSR | S_IXUSR | /* rwx */
					S_IRGRP | S_IWGRP | S_IXGRP | /* rwx */
					S_IROTH | S_IXOTH | S_IXOTH);
			if (ret == 0 || errno == EEXIST) {
				rtp_set_auto_play(state->rtp, false);
				rtp_set_is_looping(state->rtp, false);

				lg_is_converting = true;
				convert_snap_handler();

				snprintf(menu->name, 256, "StopConverting:%s", lg_convert_base_path);
				printf("start converting to %s\n", lg_convert_base_path);
			}
		} else if (rtp_is_recording(state->rtp, NULL)) { //stop record
			rtp_stop_recording(state->rtp);
			snprintf(menu->name, 8, "Record");
			printf("stop recording\n");
		} else if (!rtp_is_loading(state->rtp, NULL)) { //start record
			char dst[256];
			int last_id = get_last_id(PACKET_FOLDER_PATH);
			if (last_id >= 0) {
				snprintf(dst, 256, PACKET_FOLDER_PATH "/%d.rtp", last_id + 1);
				rtp_start_recording(state->rtp, dst);
				snprintf(menu->name, 256, "StopRecording:%s", dst);
				printf("start recording %s\n", dst);
			}
		}
		break;
	case MENU_EVENT_DESELECTED:
		break;
	case MENU_EVENT_BEFORE_DELETE:
		break;
	case MENU_EVENT_NONE:
	default:
		break;
	}
}

static void packet_menu_load_node_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_ACTIVATED:
		break;
	case MENU_EVENT_DEACTIVATED:
		break;
	case MENU_EVENT_SELECTED:
		if (lg_is_converting) {
			//do nothing
		} else if (menu->marked) {
			rtp_stop_loading(state->rtp);
			printf("stop loading\n");
			menu->marked = false;
			menu->selected = false;

			state->options.config_ex_enabled = false;
		} else if (!rtp_is_recording(state->rtp, NULL) && !rtp_is_loading(state->rtp, NULL)) {
			char filepath[256];
			snprintf(filepath, 256, PACKET_FOLDER_PATH "/%s", (char*) menu->user_data);
			rtp_start_loading(state->rtp, filepath, true, true, (RTP_LOADING_CALLBACK) loading_callback, NULL);
			printf("start loading %s\n", filepath);
			menu->marked = true;
			menu->selected = false;

			{ //try loading configuration
				snprintf(state->options.config_ex_filepath, 256, "%s.json", filepath);
				init_options_ex(state);
				state->options.config_ex_enabled = true;
			}

			snprintf(lg_convert_base_path, 256, VIDEO_FOLDER_PATH "/%s", (char*) menu->user_data);
			lg_convert_frame_num = 0;
		} else {
			menu->selected = false;
		}
		break;
	case MENU_EVENT_DESELECTED:
		break;
	case MENU_EVENT_BEFORE_DELETE:
		if (menu->user_data) {
			free(menu->user_data);
		}
		break;
	case MENU_EVENT_NONE:
	default:
		break;
	}
}

static void packet_menu_load_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_ACTIVATED:
		if (!rtp_is_loading(state->rtp, NULL)) {
			struct dirent *d;
			DIR *dir;

			dir = opendir(PACKET_FOLDER_PATH);
			if (dir != NULL) {
				while ((d = readdir(dir)) != 0) {
					if (d->d_name[0] != L'.') {
						int len = strlen(d->d_name);
						if (strncmp(d->d_name + len - 5, ".json", 5) == 0) {
							continue;
						}

						char *name = malloc(256);
						strncpy(name, d->d_name, 256);
						MENU_T *node_menu = menu_new(name, packet_menu_load_node_callback, name);
						menu_add_submenu(menu, node_menu, INT_MAX);
					}
				}
			}
		}
		break;
	case MENU_EVENT_DEACTIVATED:
		if (!rtp_is_loading(state->rtp, NULL)) {
			for (int idx = 0; menu->submenu[idx]; idx++) {
				menu_delete(&menu->submenu[idx]);
			}
		}
		break;
	case MENU_EVENT_SELECTED:
		break;
	case MENU_EVENT_DESELECTED:
		break;
	case MENU_EVENT_BEFORE_DELETE:
		break;
	case MENU_EVENT_NONE:
	default:
		break;
	}
}

static void function_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		switch ((int) menu->user_data) {
		case 0: //snap
			menu->selected = false;
			{
				char dst[256];
				int last_id = get_last_id(STILL_FOLDER_PATH);
				if (last_id >= 0) {
					snprintf(dst, 256, STILL_FOLDER_PATH "/%d.jpeg", last_id + 1);
					state->plugin_host.snap(lg_resolution * 1024, lg_resolution * 512, RENDERING_MODE_EQUIRECTANGULAR, dst);
				}
			}
			break;
		default:
			break;
		}
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void stereo_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		lg_stereo_enabled = !(bool) menu->user_data;
		menu->user_data = (void*) lg_stereo_enabled;
		if (lg_stereo_enabled) {
			snprintf(menu->name, 8, "On");
		} else {
			snprintf(menu->name, 8, "Off");
		}
		{
			char cmd[256];
			snprintf(cmd, 256, "set_stereo %d", lg_stereo_enabled ? 1 : 0);
			state->plugin_host.send_command(cmd);
		}
		menu->selected = false;
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void refraction_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		for (int idx = 0; menu->parent->submenu[idx]; idx++) {
			menu->parent->submenu[idx]->marked = false;
		}
		menu->marked = true;
		menu->selected = false;
		{
			float value = 1.0;
			switch ((int) menu->user_data) {
			case 1:
				value = 1.0;
				break;
			case 2:
				value = 0.98;
				break;
			case 3:
				value = 0.96;
				break;
			}
			char cmd[256];
			snprintf(cmd, 256, "set_camera_horizon_r_bias %f", value);
			state->plugin_host.send_command(cmd);
		}
		{
			float value = 1.0;
			switch ((int) menu->user_data) {
			case 1:
				value = 1.0;
				break;
			case 2:
				value = 1.1;
				break;
			case 3:
				value = 1.3;
				break;
			}
			state->refraction = value;
		}
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void play_speed_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		for (int idx = 0; menu->parent->submenu[idx]; idx++) {
			menu->parent->submenu[idx]->marked = false;
		}
		menu->marked = true;
		menu->selected = false;
		{
			float value = (float) ((int) menu->user_data) / 100;
			char cmd[256];
			snprintf(cmd, 256, "set_play_speed %f", value);
			state->plugin_host.send_command(cmd);
		}
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void sync_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		lg_sync_enabled = !(bool) menu->user_data;
		menu->user_data = (void*) lg_sync_enabled;
		if (lg_sync_enabled) {
			snprintf(menu->name, 8, "On");
		} else {
			snprintf(menu->name, 8, "Off");
		}
		{
			char cmd[256];
			snprintf(cmd, 256, "set_conf_sync %d", lg_sync_enabled ? 1 : 0);
			state->plugin_host.send_command(cmd);
		}
		menu->selected = false;
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void resolution_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		lg_resolution = (int) menu->user_data;
		for (int idx = 0; menu->parent->submenu[idx]; idx++) {
			menu->parent->submenu[idx]->marked = false;
		}
		menu->marked = true;
		menu->selected = false;
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void horizonr_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		if (1) {
			float value = (int) menu->user_data;
			value *= 0.01;
			char cmd[256];
			snprintf(cmd, 256, "add_camera_horizon_r *=%f", value);
			state->plugin_host.send_command(cmd);
		}
		menu->selected = false;
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void fov_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		if (1) {
			int value = (int) menu->user_data;
			if (value == -1 || value == 1) {
				value = state->plugin_host.get_fov() + value;
			}
			state->plugin_host.set_fov(MIN(MAX(value, 60), 120));
		}
		menu->selected = false;
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void calibration_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		switch ((int) menu->user_data) {
		case CALIBRATION_CMD_SAVE:
			menu->selected = false;
			{
				char cmd[256];
				snprintf(cmd, 256, "save");
				state->plugin_host.send_command(cmd);
			}
			break;
		case CALIBRATION_CMD_IMAGE_CIRCLE:
			if (1) {
				if (state->frame) {
					state->frame->tmp_renderer = state->frame->renderer;
					state->frame->renderer = get_renderer("CALIBRATION");
				}
			}
			break;
		case CALIBRATION_CMD_VIEWER_COMPASS:
			if (1) {
				char cmd[256];
				snprintf(cmd, 256, "mpu9250.start_compass_calib");
				state->plugin_host.send_command(cmd);
				//snprintf(cmd, 256, "oculus_rift_dk2.start_compass_calib");
				//state->plugin_host.send_command(cmd);
			}
			break;
		case CALIBRATION_CMD_VEHICLE_COMPASS:
			if (1) { //this should be in plugin
				char cmd[256];
				snprintf(cmd, 256, "upstream.mpu9250.start_compass_calib");
				state->plugin_host.send_command(cmd);
			}
			break;
		default:
			break;
		}
		break;
	case MENU_EVENT_DESELECTED:
		switch ((int) menu->user_data) {
		case CALIBRATION_CMD_IMAGE_CIRCLE:
			if (1) {
				if (state->frame) {
					state->frame->renderer = state->frame->tmp_renderer;
				}
			}
			break;
		case CALIBRATION_CMD_VIEWER_COMPASS:
			if (1) {
				char cmd[256];
				snprintf(cmd, 256, "mpu9250.stop_compass_calib");
				state->plugin_host.send_command(cmd);
				//snprintf(cmd, 256, "oculus_rift_dk2.stop_compass_calib");
				//state->plugin_host.send_command(cmd);
			}
			break;
		case CALIBRATION_CMD_VEHICLE_COMPASS:
			if (1) {
				char cmd[256];
				snprintf(cmd, 256, "upstream.mpu9250.stop_compass_calib");
				state->plugin_host.send_command(cmd);
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

static void image_circle_calibration_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	const float GAIN = 0.005;
	switch (event) {
	case MENU_EVENT_SELECTED:
		switch ((int) menu->user_data) {
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
		case 8:
		case 9:
			if (1) {
				char name[64];
				sprintf(name, "%d", (int) menu->user_data - 1);
				state->plugin_host.send_command(name);
			}
			menu->selected = false;
			break;
		case 10:				//x
			break;
		case 11:				//decrement x
			if (1) {
				float value = (int) 1;
				value *= GAIN;
				char cmd[256];
				snprintf(cmd, 256, "add_camera_offset_x %d=%f", state->active_cam, value);
				state->plugin_host.send_command(cmd);
			}
			menu->selected = false;
			break;
		case 12:				//increment x
			if (1) {
				float value = (int) -1;
				value *= GAIN;
				char cmd[256];
				snprintf(cmd, 256, "add_camera_offset_x %d=%f", state->active_cam, value);
				state->plugin_host.send_command(cmd);
			}
			menu->selected = false;
			break;
		case 20:				//y
			break;
		case 21:				//decrement y
			if (1) {
				float value = (int) 1;
				value *= GAIN;
				char cmd[256];
				snprintf(cmd, 256, "add_camera_offset_y %d=%f", state->active_cam, value);
				state->plugin_host.send_command(cmd);
			}
			menu->selected = false;
			break;
		case 22:				//increment y
			if (1) {
				float value = (int) -1;
				value *= GAIN;
				char cmd[256];
				snprintf(cmd, 256, "add_camera_offset_y %d=%f", state->active_cam, value);
				state->plugin_host.send_command(cmd);
			}
			menu->selected = false;
			break;
		case 30:				//start ac
			if (1) {
				char cmd[256];
				snprintf(cmd, 256, "start_ac");
				state->plugin_host.send_command(cmd);
			}
			break;
		default:
			break;
		}
		break;
	case MENU_EVENT_DESELECTED:
		switch ((int) menu->user_data) {
		case 30:
			if (1) {
				char cmd[256];
				snprintf(cmd, 256, "stop_ac");
				state->plugin_host.send_command(cmd);
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

static void image_params_calibration_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		if (menu->user_data) {
			state->plugin_host.send_command((char*) menu->user_data);
			menu->selected = false;
		}
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void system_menu_callback(struct _MENU_T *menu, enum MENU_EVENT event) {
	switch (event) {
	case MENU_EVENT_SELECTED:
		if ((enum SYSTEM_CMD) menu->user_data == SYSTEM_CMD_SHUTDOWN) {
			system("sudo shutdown now");
			exit(1);
		} else if ((enum SYSTEM_CMD) menu->user_data == SYSTEM_CMD_REBOOT) {
			system("sudo reboot");
			exit(1);
		} else if ((enum SYSTEM_CMD) menu->user_data == SYSTEM_CMD_EXIT) {
			exit(1);
		}
		break;
	case MENU_EVENT_DESELECTED:
		break;
	default:
		break;
	}
}

static void _init_menu() {
	init_menu(state->screen_height / 32);
	state->menu = menu_new("Menu", menu_callback, NULL);

	MENU_T *menu = state->menu;
	{
		MENU_T *sub_menu = menu_add_submenu(menu, menu_new("Function", NULL, NULL), INT_MAX);
		menu_add_submenu(sub_menu, menu_new("Snap", function_menu_callback, (void*) 0), INT_MAX);
	}
	{
		MENU_T *sub_menu = menu_add_submenu(menu, menu_new("Config", NULL, NULL), INT_MAX);
		MENU_T *sync_menu = menu_add_submenu(sub_menu, menu_new("Sync", NULL, NULL), INT_MAX);
		{
			MENU_T *sub_menu = sync_menu;
			MENU_T *on_menu = menu_add_submenu(sub_menu, menu_new("On", sync_menu_callback, (void*) true), INT_MAX);
			on_menu->marked = true;
		}
		MENU_T *refraction_menu = menu_add_submenu(sub_menu, menu_new("Refraction", NULL, NULL), INT_MAX);
		{
			MENU_T *sub_menu = refraction_menu;
			menu_add_submenu(sub_menu, menu_new("Air", refraction_menu_callback, (void*) 1), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("AcrylicDome", refraction_menu_callback, (void*) 2), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("UnderWater", refraction_menu_callback, (void*) 3), INT_MAX);
			refraction_menu->submenu[0]->marked = true;
		}
		MENU_T *stereo_menu = menu_add_submenu(sub_menu, menu_new("Stereo", NULL, NULL), INT_MAX);
		{
			MENU_T *sub_menu = stereo_menu;
			MENU_T *off_menu = menu_add_submenu(sub_menu, menu_new("Off", stereo_menu_callback, (void*) false), INT_MAX);
			off_menu->marked = true;
		}
		MENU_T *resolution_menu = menu_add_submenu(sub_menu, menu_new("Resolution", NULL, NULL), INT_MAX);
		{
			MENU_T *sub_menu = resolution_menu;
			menu_add_submenu(sub_menu, menu_new("4K", resolution_menu_callback, (void*) 4), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("3K", resolution_menu_callback, (void*) 3), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("2K", resolution_menu_callback, (void*) 2), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("1K", resolution_menu_callback, (void*) 1), INT_MAX);
			for (int idx = 0; sub_menu->submenu[idx]; idx++) {
				if (sub_menu->submenu[idx]->user_data == (void*) lg_resolution) {
					sub_menu->submenu[idx]->marked = true;
				}
			}
		}
		MENU_T *play_speed_menu = menu_add_submenu(sub_menu, menu_new("PlaySpeed", NULL, NULL), INT_MAX);
		{
			MENU_T *sub_menu = play_speed_menu;
			menu_add_submenu(sub_menu, menu_new("x1/8", play_speed_menu_callback, (void*) 12), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("x1/4", play_speed_menu_callback, (void*) 25), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("x1/2", play_speed_menu_callback, (void*) 50), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("x1", play_speed_menu_callback, (void*) 100), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("x2", play_speed_menu_callback, (void*) 200), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("x4", play_speed_menu_callback, (void*) 400), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("x8", play_speed_menu_callback, (void*) 400), INT_MAX);
			for (int idx = 0; sub_menu->submenu[idx]; idx++) {
				int play_speed_percent = (int) (state->rtp_play_speed * 100);
				if (sub_menu->submenu[idx]->user_data == (void*) play_speed_percent) {
					sub_menu->submenu[idx]->marked = true;
				}
			}
		}
		MENU_T *fov_menu = menu_add_submenu(sub_menu, menu_new("Fov", NULL, NULL), INT_MAX);
		{
			MENU_T *sub_menu = fov_menu;
			menu_add_submenu(sub_menu, menu_new("-", fov_menu_callback, (void*) -1), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("+", fov_menu_callback, (void*) 1), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("60", fov_menu_callback, (void*) 60), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("90", fov_menu_callback, (void*) 90), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("120", fov_menu_callback, (void*) 120), INT_MAX);
		}
	}
	{
		MENU_T *sub_menu = menu_add_submenu(menu, menu_new("Packet", NULL, NULL), INT_MAX);
		menu_add_submenu(sub_menu, menu_new("Record", packet_menu_record_callback, NULL), INT_MAX);
		menu_add_submenu(sub_menu, menu_new("Play", packet_menu_load_callback, NULL), INT_MAX);
	}
	{
		MENU_T *sub_menu = menu_add_submenu(menu, menu_new("Calibration", NULL, NULL), INT_MAX);
		MENU_T *image_circle_menu = menu_add_submenu(sub_menu, menu_new("ImageCircle", calibration_menu_callback, (void*) CALIBRATION_CMD_IMAGE_CIRCLE), INT_MAX);
		{
			MENU_T *sub_menu = image_circle_menu;
			MENU_T *image_circle_cam_menu = menu_add_submenu(sub_menu, menu_new("cam", image_circle_calibration_menu_callback, (void*) 0), INT_MAX);
			{
				MENU_T *sub_menu = image_circle_cam_menu;
				for (int i = 0; i < state->num_of_cam; i++) {
					char name[64];
					sprintf(name, "%d", i);
					menu_add_submenu(sub_menu, menu_new(name, image_circle_calibration_menu_callback, (void*) i + 1), INT_MAX);
				}
			}
			MENU_T *image_circle_x_menu = menu_add_submenu(sub_menu, menu_new("x", image_circle_calibration_menu_callback, (void*) 10), INT_MAX);
			{
				MENU_T *sub_menu = image_circle_x_menu;
				menu_add_submenu(sub_menu, menu_new("-", image_circle_calibration_menu_callback, (void*) 11), INT_MAX);
				menu_add_submenu(sub_menu, menu_new("+", image_circle_calibration_menu_callback, (void*) 12), INT_MAX);
			}
			MENU_T *image_circle_y_menu = menu_add_submenu(sub_menu, menu_new("y", image_circle_calibration_menu_callback, (void*) 20), INT_MAX);
			{
				MENU_T *sub_menu = image_circle_y_menu;
				menu_add_submenu(sub_menu, menu_new("-", image_circle_calibration_menu_callback, (void*) 21), INT_MAX);
				menu_add_submenu(sub_menu, menu_new("+", image_circle_calibration_menu_callback, (void*) 22), INT_MAX);
			}
			menu_add_submenu(sub_menu, menu_new("ac", image_circle_calibration_menu_callback, (void*) 30), INT_MAX);
		}
		MENU_T *image_params_menu = menu_add_submenu(sub_menu, menu_new("ImageParams", calibration_menu_callback, (void*) CALIBRATION_CMD_IMAGE_PARAMS), INT_MAX);
		{
			MENU_T *sub_menu = image_params_menu;
			{
				MENU_T *_sub_menu = menu_add_submenu(sub_menu, menu_new("brightness", image_params_calibration_menu_callback, NULL), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("-", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl brightness=-1"), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("+", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl brightness=1"), INT_MAX);
			}
			{
				MENU_T *_sub_menu = menu_add_submenu(sub_menu, menu_new("gamma", image_params_calibration_menu_callback, NULL), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("-", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl gamma=-1"), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("+", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl gamma=1"), INT_MAX);
			}
			{
				MENU_T *_sub_menu = menu_add_submenu(sub_menu, menu_new("contrast", image_params_calibration_menu_callback, NULL), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("-", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl contrast=-1"), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("+", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl contrast=1"), INT_MAX);
			}
			{
				MENU_T *_sub_menu = menu_add_submenu(sub_menu, menu_new("saturation", image_params_calibration_menu_callback, NULL), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("-", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl saturation=-1"), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("+", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl saturation=1"), INT_MAX);
			}
			{
				MENU_T *_sub_menu = menu_add_submenu(sub_menu, menu_new("sharpness", image_params_calibration_menu_callback, NULL), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("-", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl sharpness=-1"), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("+", image_params_calibration_menu_callback, (void*) ENDPOINT_DOMAIN "v4l2_capture.add_v4l2_ctl sharpness=1"), INT_MAX);
			}
//			{
//				MENU_T *_sub_menu = menu_add_submenu(sub_menu, menu_new("white_balance_temperature_auto", image_params_calibration_menu_callback, NULL), INT_MAX);
//				menu_add_submenu(_sub_menu, menu_new("-", image_params_calibration_menu_callback, NULL), INT_MAX);
//				menu_add_submenu(_sub_menu, menu_new("+", image_params_calibration_menu_callback, NULL), INT_MAX);
//			}
//			{
//				MENU_T *_sub_menu = menu_add_submenu(sub_menu, menu_new("white_balance_temperature", image_params_calibration_menu_callback, NULL), INT_MAX);
//				menu_add_submenu(_sub_menu, menu_new("-", image_params_calibration_menu_callback, NULL), INT_MAX);
//				menu_add_submenu(_sub_menu, menu_new("+", image_params_calibration_menu_callback, NULL), INT_MAX);
//			}
//			{
//				MENU_T *_sub_menu = menu_add_submenu(sub_menu, menu_new("exposure_auto", image_params_calibration_menu_callback, NULL), INT_MAX);
//				menu_add_submenu(_sub_menu, menu_new("-", image_params_calibration_menu_callback, NULL, INT_MAX);
//				menu_add_submenu(_sub_menu, menu_new("+", image_params_calibration_menu_callback, NULL), INT_MAX);
//			}
			{
				MENU_T *_sub_menu = menu_add_submenu(sub_menu, menu_new("color_offset", image_params_calibration_menu_callback, NULL), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("-", image_params_calibration_menu_callback, (void*) "add_color_offset -0.01"), INT_MAX);
				menu_add_submenu(_sub_menu, menu_new("+", image_params_calibration_menu_callback, (void*) "add_color_offset 0.01"), INT_MAX);
			}
		}
		menu_add_submenu(sub_menu, menu_new("ViewerCompass", calibration_menu_callback, (void*) CALIBRATION_CMD_VIEWER_COMPASS), INT_MAX);
		menu_add_submenu(sub_menu, menu_new("VehicleCompass", calibration_menu_callback, (void*) CALIBRATION_CMD_VEHICLE_COMPASS), INT_MAX);
		MENU_T *horizonr_menu = menu_add_submenu(sub_menu, menu_new("HorizonR", NULL, NULL), 0);
		{
			MENU_T *sub_menu = horizonr_menu;
			menu_add_submenu(sub_menu, menu_new("-", horizonr_menu_callback, (void*) -1), INT_MAX);
			menu_add_submenu(sub_menu, menu_new("+", horizonr_menu_callback, (void*) 1), INT_MAX);
		}
		menu_add_submenu(sub_menu, menu_new("Save", calibration_menu_callback, (void*) CALIBRATION_CMD_SAVE), INT_MAX);
	}
	{
		MENU_T *sub_menu = menu_add_submenu(menu, menu_new("System", NULL, NULL), INT_MAX);
		menu_add_submenu(sub_menu, menu_new("Shutdown", system_menu_callback, (void*) SYSTEM_CMD_SHUTDOWN), INT_MAX);
		menu_add_submenu(sub_menu, menu_new("Reboot", system_menu_callback, (void*) SYSTEM_CMD_REBOOT), INT_MAX);
		menu_add_submenu(sub_menu, menu_new("Exit", system_menu_callback, (void*) SYSTEM_CMD_EXIT), INT_MAX);
	}
}

#endif //menu block

int main(int argc, char *argv[]) {

	//create_oculus_rift_dk2(NULL, NULL); //this should be first, pthread_create is related

	bool input_file_mode = false;
	int opt;
	char frame_param[256] = { };

	// Clear application state
	const int INITIAL_SPACE = 16;
	memset(state, 0, sizeof(*state));
	state->cam_width = 2048;
	state->cam_height = 2048;
	state->num_of_cam = 1;
	state->preview = false;
	state->stereo = false;
	strncpy(state->mpu_name, "manual", sizeof(state->mpu_name));
	strncpy(state->capture_name, "ffmpeg", sizeof(state->capture_name));
	strncpy(state->decoder_name, "ffmpeg", sizeof(state->decoder_name));
	strncpy(state->audio_capture_name, "none", sizeof(state->audio_capture_name));
	state->video_direct = false;
	state->input_mode = INPUT_MODE_CAM;
	state->output_raw = false;
	state->conf_sync = true;
	state->camera_horizon_r_bias = 1.0;
	state->refraction = 1.0;
	state->rtp_play_speed = 1.0;
	strncpy(state->default_view_coordinate_mode, "manual", sizeof(state->default_view_coordinate_mode));
	strncpy(state->config_filepath, "config.json", sizeof(state->config_filepath));

	{
		state->plugins = malloc(sizeof(PLUGIN_T*) * INITIAL_SPACE);
		memset(state->plugins, 0, sizeof(PLUGIN_T*) * INITIAL_SPACE);
		state->plugins[INITIAL_SPACE - 1] = (void*) -1;
	}
	{
		state->mpu_factories = malloc(sizeof(MPU_FACTORY_T*) * INITIAL_SPACE);
		memset(state->mpu_factories, 0, sizeof(MPU_FACTORY_T*) * INITIAL_SPACE);
		state->mpu_factories[INITIAL_SPACE - 1] = (void*) -1;
	}
	{
		state->capture_factories = malloc(sizeof(CAPTURE_FACTORY_T*) * INITIAL_SPACE);
		memset(state->capture_factories, 0, sizeof(CAPTURE_FACTORY_T*) * INITIAL_SPACE);
		state->capture_factories[INITIAL_SPACE - 1] = (void*) -1;
	}
	{
		state->decoder_factories = malloc(sizeof(DECODER_FACTORY_T*) * INITIAL_SPACE);
		memset(state->decoder_factories, 0, sizeof(DECODER_FACTORY_T*) * INITIAL_SPACE);
		state->decoder_factories[INITIAL_SPACE - 1] = (void*) -1;
	}
	{
		state->encoder_factories = malloc(sizeof(ENCODER_FACTORY_T*) * INITIAL_SPACE);
		memset(state->encoder_factories, 0, sizeof(ENCODER_FACTORY_T*) * INITIAL_SPACE);
		state->encoder_factories[INITIAL_SPACE - 1] = (void*) -1;
	}
	{
		state->renderers = malloc(sizeof(RENDERER_T*) * INITIAL_SPACE);
		memset(state->renderers, 0, sizeof(RENDERER_T*) * INITIAL_SPACE);
		state->renderers[INITIAL_SPACE - 1] = (void*) -1;
	}
	{
		state->watches = malloc(sizeof(STATUS_T*) * INITIAL_SPACE);
		memset(state->watches, 0, sizeof(STATUS_T*) * INITIAL_SPACE);
		state->watches[INITIAL_SPACE - 1] = (void*) -1;
	}
	{
		state->statuses = malloc(sizeof(STATUS_T*) * INITIAL_SPACE);
		memset(state->statuses, 0, sizeof(STATUS_T*) * INITIAL_SPACE);
		state->statuses[INITIAL_SPACE - 1] = (void*) -1;
	}

	umask(0000);

	optind = 1; // reset getopt
	while ((opt = getopt(argc, argv, "c:psi:r:F:v:")) != -1) {
		switch (opt) {
		case 'c':
			strncpy(state->config_filepath, optarg, sizeof(state->config_filepath));
			break;
		case 'p':
			state->preview = true;
			break;
		case 's':
			state->stereo = true;
			break;
		case 'r':
			state->output_raw = true;
			strncpy(state->output_raw_filepath, optarg, sizeof(state->output_raw_filepath));
			break;
		case 'i':
			strncpy(state->input_filepath, optarg, sizeof(state->input_filepath));
			state->input_mode = INPUT_MODE_FILE;
			state->input_file_cur = -1;
			state->input_file_size = 0;
			state->frame_sync = true;
			input_file_mode = true;
			break;
		case 'F':
			strncpy(frame_param, optarg, 256);
			break;
		case 'v':
			strncpy(state->default_view_coordinate_mode, optarg, 64);
			break;
		default:
			/* '?' */
			printf("Usage: %s [-c conf_filepath] [-p] [-s]\n", argv[0]);
			return -1;
		}
	}

	//init options
	init_options(state);

	{ //mrevent & mutex init
		for (int i = 0; i < state->num_of_cam; i++) {
			mrevent_init(&state->request_frame_event[i]);
			mrevent_trigger(&state->request_frame_event[i]);
			mrevent_init(&state->arrived_frame_event[i]);
			mrevent_reset(&state->arrived_frame_event[i]);
		}

		pthread_mutex_init(&state->mutex, 0);
		//texture mutex init
		pthread_mutex_init(&state->texture_mutex, 0);
		//texture size mutex init
		pthread_mutex_init(&state->texture_size_mutex, 0);
		//frame mutex init
		pthread_mutex_init(&state->cmd_list_mutex, 0);
	}
#if BCM_HOST
	bcm_host_init();
	printf("Note: ensure you have sufficient gpu_mem configured\n");
#endif

	// Start OGLES
	init_ogl(state);

	//menu
	_init_menu();

	// init plugin
	init_plugins(state);

	// Setup the model world
	init_model_proj(state);

	//init rtp
	_init_rtp(state);

	// initialise the OGLES texture(s)
	init_textures(state);

	//frame id=0
	if (frame_param[0]) {
		char cmd[256];
		sprintf(cmd, "create_frame %s", frame_param);
		state->plugin_host.send_command(cmd);
	}
	//set mpu
	for (int i = 0; state->mpu_factories[i] != NULL; i++) {
		if (strncmp(state->mpu_factories[i]->name, state->mpu_name, 64) == 0) {
			state->mpu_factories[i]->create_mpu(state->mpu_factories[i]->user_data, &state->mpu);
		}
	}
	if (state->mpu == NULL) {
		printf("something wrong with %s\n", state->mpu_name);
	}

	static struct timeval last_time = { };
	gettimeofday(&last_time, NULL);

	static struct timeval last_statuses_handled_time = { };
	gettimeofday(&last_statuses_handled_time, NULL);

	//readline
	pthread_t readline_thread;
	pthread_create(&readline_thread, NULL, readline_thread_func, (void*) NULL);

	//quaternion
	pthread_t quaternion_thread;
	pthread_create(&quaternion_thread, NULL, quaternion_thread_func, (void*) NULL);

	//status buffer
	char *status_value = (char*) malloc(RTP_MAXPAYLOADSIZE);
	char *status_buffer = (char*) malloc(RTP_MAXPAYLOADSIZE);
	char *status_packet = (char*) malloc(RTP_MAXPAYLOADSIZE);

	while (!terminate) {
		struct timeval time = { };
		gettimeofday(&time, NULL);

		if (state->frame_sync) {
			int res = 0;
			for (int i = 0; i < state->num_of_cam; i++) {
				int res = mrevent_wait(&state->arrived_frame_event[i], 1000); //wait 1msec
				if (res != 0) {
					break;
				}
			}
			if (res != 0) {
				continue; // skip
			}
		}
		frame_handler();
		command_handler();
		command2upstream_handler();
		if (state->frame) {
			for (int i = 0; i < state->num_of_cam; i++) {
				mrevent_reset(&state->arrived_frame_event[i]);
				mrevent_trigger(&state->request_frame_event[i]);
			}
		}
		if (input_file_mode && state->input_file_cur == state->input_file_size) {
			terminate = true;
		}

		{ //wait 10msec at least for performance
			struct timeval diff;
			timersub(&time, &last_time, &diff);
			float diff_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;
			if (diff_sec < 0.010) { //10msec
				int delay_ms = 10 - (int) (diff_sec * 1000);
				usleep(delay_ms * 1000);
			}
		}
		{ //status
			struct timeval diff;
			timersub(&time, &last_statuses_handled_time, &diff);
			float diff_sec = (float) diff.tv_sec + (float) diff.tv_usec / 1000000;
			if (diff_sec > 0.100) { //less than 10Hz
				int cur = 0;
				for (int i = 0; state->statuses[i]; i++) {
					state->statuses[i]->get_value(state->statuses[i]->user_data, status_value, RTP_MAXPAYLOADSIZE);
					int len = snprintf(status_buffer, RTP_MAXPAYLOADSIZE, "<picam360:status name=\"%s\" value=\"%s\" />", state->statuses[i]->name, status_value);
					if (cur != 0 && cur + len > RTP_MAXPAYLOADSIZE) {
						rtp_sendpacket(state->rtp, (unsigned char*) status_packet, cur, PT_STATUS);
						cur = 0;
					} else {
						strncpy(status_packet + cur, status_buffer, len);
						cur += len;
					}
				}
				if (cur != 0) {
					rtp_sendpacket(state->rtp, (unsigned char*) status_packet, cur, PT_STATUS);
				}
				last_statuses_handled_time = time;
			}
		}
		last_time = time;
	}
	exit_func();
	return 0;
}

/***********************************************************
 * Name: redraw_scene
 *
 * Arguments:
 *       PICAM360CAPTURE_T *state - holds OGLES model info
 *
 * Description:   Draws the model and calls eglSwapBuffers
 *                to render to screen
 *
 * Returns: void
 *
 ***********************************************************/
static void redraw_render_texture(PICAM360CAPTURE_T *state, FRAME_T *frame, RENDERER_T *renderer, VECTOR4D_T view_quat) {
	if (renderer == NULL) {
		return;
	}

	int frame_width = (state->stereo) ? frame->width / 2 : frame->width;
	int frame_height = frame->height;

	int program = renderer->get_program(renderer);
	glUseProgram(program);

	glViewport(0, 0, frame_width, frame_height);

	{ // bind texture
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, state->logo_texture);
		glUniform1i(glGetUniformLocation(program, "logo_texture"), 0);

		int cam_texture[MAX_CAM_NUM];
		for (int i = 0; i < state->num_of_cam; i++) {
//			if (state->decoders[i]) {
//				state->decoders[i]->switch_buffer(state->decoders[i]);//wait texture updated
//			}
			cam_texture[i] = i + 1;
			glActiveTexture(GL_TEXTURE1 + i);
			glBindTexture(GL_TEXTURE_2D, state->cam_texture[i][state->cam_texture_cur[i]]);
		}
		glUniform1iv(glGetUniformLocation(program, "cam_texture"), state->num_of_cam, cam_texture);
	}

	{ //cam_attitude //depth axis is z, vertical asis is y
		float cam_attitude[16 * MAX_CAM_NUM];
		float view_matrix[16];
		float north_matrix[16];
		float world_matrix[16];

		{ // Rv : view
			mat4_identity(view_matrix);
			mat4_fromQuat(view_matrix, view_quat.ary);
			mat4_invert(view_matrix, view_matrix);
		}

		{ // Rn : north
			mat4_identity(north_matrix);
			float north = state->plugin_host.get_view_north();
			mat4_rotateY(north_matrix, north_matrix, north * M_PI / 180);
		}

		{ // Rw : view coodinate to world coodinate and view heading to ground initially
			mat4_identity(world_matrix);
			mat4_rotateX(world_matrix, world_matrix, -M_PI / 2);
		}

		for (int i = 0; i < state->num_of_cam; i++) {
			float *unif_matrix = cam_attitude + 16 * i;
			float cam_matrix[16];
			{ // Rc : cam orientation
				mat4_identity(cam_matrix);
				if (state->camera_coordinate_from_device) {
					mat4_fromQuat(cam_matrix, state->camera_quaternion[i].ary);
				} else {
					//euler Y(yaw)X(pitch)Z(roll)
					mat4_rotateZ(cam_matrix, cam_matrix, state->camera_roll);
					mat4_rotateX(cam_matrix, cam_matrix, state->camera_pitch);
					mat4_rotateY(cam_matrix, cam_matrix, state->camera_yaw);
				}
			}

			{ // Rco : cam offset  //euler Y(yaw)X(pitch)Z(roll)
				float cam_offset_matrix[16];
				mat4_identity(cam_offset_matrix);
				mat4_rotateZ(cam_offset_matrix, cam_offset_matrix, state->options.cam_offset_roll[i]);
				mat4_rotateX(cam_offset_matrix, cam_offset_matrix, state->options.cam_offset_pitch[i]);
				mat4_rotateY(cam_offset_matrix, cam_offset_matrix, state->options.cam_offset_yaw[i]);
				mat4_invert(cam_offset_matrix, cam_offset_matrix);
				mat4_multiply(cam_matrix, cam_matrix, cam_offset_matrix); // Rc'=RcoRc
			}

			{ //RcRv(Rc^-1)RcRw
				mat4_identity(unif_matrix);
				mat4_multiply(unif_matrix, unif_matrix, world_matrix); // Rw
				mat4_multiply(unif_matrix, unif_matrix, view_matrix); // RvRw
				//mat4_multiply(unif_matrix, unif_matrix, north_matrix); // RnRvRw
				mat4_multiply(unif_matrix, unif_matrix, cam_matrix); // RcRnRvRw
			}
			mat4_transpose(unif_matrix, unif_matrix); // this mat4 library is row primary, opengl is column primary
		}
		glUniformMatrix4fv(glGetUniformLocation(program, "cam_attitude"), state->num_of_cam, GL_FALSE, (GLfloat*) cam_attitude);
	}
	{ //cam_options
		float cam_offset_yaw[MAX_CAM_NUM];
		float cam_offset_x[MAX_CAM_NUM];
		float cam_offset_y[MAX_CAM_NUM];
		float cam_horizon_r[MAX_CAM_NUM];
		float cam_aov[MAX_CAM_NUM];
		for (int i = 0; i < state->num_of_cam; i++) {
			cam_offset_x[i] = state->options.cam_offset_x[i];
			cam_offset_y[i] = state->options.cam_offset_y[i];
			cam_horizon_r[i] = state->options.cam_horizon_r[i];
			cam_aov[i] = state->options.cam_aov[i];
			if (state->options.config_ex_enabled) {
				cam_offset_x[i] += state->options.cam_offset_x_ex[i];
				cam_offset_y[i] += state->options.cam_offset_y_ex[i];
				cam_horizon_r[i] += state->options.cam_horizon_r_ex[i];
			}
			cam_horizon_r[i] *= state->camera_horizon_r_bias;
			cam_aov[i] /= state->refraction;
		}
		glUniform1fv(glGetUniformLocation(program, "cam_offset_x"), state->num_of_cam, cam_offset_x);
		glUniform1fv(glGetUniformLocation(program, "cam_offset_y"), state->num_of_cam, cam_offset_y);
		glUniform1fv(glGetUniformLocation(program, "cam_horizon_r"), state->num_of_cam, cam_horizon_r);
		glUniform1fv(glGetUniformLocation(program, "cam_aov"), state->num_of_cam, cam_aov);

		glUniform1i(glGetUniformLocation(program, "active_cam"), state->active_cam);
		glUniform1i(glGetUniformLocation(program, "num_of_cam"), state->num_of_cam);
	}

	//these should be into each plugin
	//Load in the texture and thresholding parameters.
	glUniform1f(glGetUniformLocation(program, "split"), state->split);
	glUniform1f(glGetUniformLocation(program, "pixel_size"), 1.0 / state->cam_width);

	glUniform1f(glGetUniformLocation(program, "cam_aspect_ratio"), (float) state->cam_width / (float) state->cam_height);
	glUniform1f(glGetUniformLocation(program, "frame_aspect_ratio"), (float) frame_width / (float) frame_height);

	glUniform1f(glGetUniformLocation(program, "sharpness_gain"), state->options.sharpness_gain);
	glUniform1f(glGetUniformLocation(program, "color_offset"), state->options.color_offset);
	glUniform1f(glGetUniformLocation(program, "color_factor"), 1.0 / (1.0 - state->options.color_offset));
	glUniform1f(glGetUniformLocation(program, "overlap"), state->options.overlap);

	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);

	renderer->render(renderer, frame->fov);

	glFlush();
}

static void redraw_scene(PICAM360CAPTURE_T *state, FRAME_T *frame, MODEL_T *model) {
	int frame_width = (state->stereo) ? frame->width / 2 : frame->width;
	int frame_height = frame->height;

	int program = GLProgram_GetId(model->program);
	glUseProgram(program);

	// Start with a clear screen
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glBindBuffer(GL_ARRAY_BUFFER, model->vbo);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, frame->texture);

	//Load in the texture and thresholding parameters.
	glUniform1i(glGetUniformLocation(program, "tex"), 0);
	glUniform1f(glGetUniformLocation(program, "tex_scalex"), (state->stereo) ? 0.5 : 1.0);

#ifdef USE_GLES
	GLuint loc = glGetAttribLocation(program, "vPosition");
	glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(loc);
#else
	glBindVertexArray(model->vao);
#endif

	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);

	if (strcasecmp(frame->renderer->name, "CALIBRATION") == 0) {
		glViewport((state->screen_width - state->screen_height) / 2, 0, (GLsizei) state->screen_height, (GLsizei) state->screen_height);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, model->vbo_nop);
	} else if (state->stereo) {
		int offset_x = (state->screen_width / 2 - frame_width) / 2;
		int offset_y = (state->screen_height - frame_height) / 2;
		for (int i = 0; i < 2; i++) {
			//glViewport(0, 0, (GLsizei)state->screen_width/2, (GLsizei)state->screen_height);
			glViewport(offset_x + i * state->screen_width / 2, offset_y, (GLsizei) frame_width, (GLsizei) frame_height);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, model->vbo_nop);
		}
	} else {
		int offset_x = (state->screen_width - frame_width) / 2;
		int offset_y = (state->screen_height - frame_height) / 2;
		glViewport(offset_x, offset_y, (GLsizei) frame_width, (GLsizei) frame_height);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, model->vbo_nop);
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
#ifdef USE_GLES
	glDisableVertexAttribArray(loc);
#else
	glBindVertexArray(0);
#endif
}

static void get_info_str(char *buff, int buff_len) {
	int len = 0;
	{ //View
		float north;
		VECTOR4D_T quat = state->plugin_host.get_view_quaternion();
		quaternion_get_euler(quat, &north, NULL, NULL, EULER_SEQUENCE_YXZ);
		len += snprintf(buff + len, buff_len - len, "View   : Tmp %.1f degC, N %.1f", state->plugin_host.get_view_temperature(), north * 180 / M_PI);
	}
	{ //Vehicle
		float north;
		VECTOR4D_T quat = state->plugin_host.get_camera_quaternion(-1);
		quaternion_get_euler(quat, &north, NULL, NULL, EULER_SEQUENCE_YXZ);
		len += snprintf(buff + len, buff_len - len, "\nVehicle: Tmp %.1f degC, N %.1f, rx %.1f Mbps, fps %.1f:%.1f skip %.0f:%.0f", state->plugin_host.get_camera_temperature(), north * 180 / M_PI,
				lg_cam_bandwidth, lg_cam_fps[0], lg_cam_fps[1], lg_cam_frameskip[0], lg_cam_frameskip[1]);
	}
	for (int i = 0; state->plugins[i] != NULL; i++) {
		if (state->plugins[i]->get_info) {
			char *info = state->plugins[i]->get_info(state->plugins[i]->user_data);
			if (info) {
				len += snprintf(buff + len, buff_len - len, "\n%s", info);
			}
		}
	}
	len += snprintf(buff + len, buff_len - len, "\n");
}

static void _get_menu_str(char **buff, int buff_len, MENU_T *menu, int depth) {
	int len = snprintf(*buff, buff_len, "%s,%d,%d,%d,%d\n", menu->name, depth, menu->activated ? 1 : 0, menu->selected ? 1 : 0, menu->marked ? 1 : 0);
	*buff += len;
	buff_len -= len;
	depth++;
	if (menu->selected) {
		for (int idx = 0; menu->submenu[idx]; idx++) {
			_get_menu_str(buff, buff_len, menu->submenu[idx], depth);
		}
	}
}

static void get_menu_str(char *_buff, int buff_len) {
	char **buff = &_buff;
	int len = snprintf(*buff, buff_len, "name,depth,activated,selected,marked\n");
	*buff += len;
	buff_len -= len;
	_get_menu_str(buff, buff_len, state->menu, 0);
}

static void redraw_info(PICAM360CAPTURE_T *state, FRAME_T *frame) {
	int frame_width = (state->stereo) ? frame->width / 2 : frame->width;
	int frame_height = frame->height;
	const int MAX_INFO_SIZE = 1024;
	char disp[MAX_INFO_SIZE];
	get_info_str(disp, MAX_INFO_SIZE);
	menu_redraw(state->menu, disp, frame_width, frame_height, frame_width, frame_height, false);
}

