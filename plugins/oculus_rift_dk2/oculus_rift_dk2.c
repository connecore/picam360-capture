#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <libovr_nsb/OVR.h>

#include "oculus_rift_dk2.h"

#define PLUGIN_NAME "oculus_rift_dk2"
#define MPU_NAME "oculus_rift_dk2"

static PLUGIN_HOST_T *lg_plugin_host = NULL;
static MPU_T *lg_mpu = NULL;

static Device *dev = NULL;
static VECTOR4D_T quat;

/////////////////////////////////////////////////////////////////////////////////////
// Continuous sample/update thread code
// select() chosen for portability
/////////////////////////////////////////////////////////////////////////////////////
static void *threadFunc(void *data) {
	Device *localDev = (Device *) data;

	while (localDev->runSampleThread) {
		// Try to sample the device for 1ms
		waitSampleDevice(localDev, 1000);

		//printf("\tQ:%+-10g %+-10g %+-10g %+-10g\n", localDev->Q[0], localDev->Q[1], localDev->Q[2], localDev->Q[3] );

		// Send a keepalive - this is too often.  Need to only send on keepalive interval
		sendSensorKeepAlive(localDev);
	}
	return 0;
}

static bool is_init = false;

static void init() {
	if (is_init) {
		return;
	} else {
		is_init = true;
	}
	//reset quat x=90 y=0 z=0
	{
		quat.ary[0] = 0.70711;
		quat.ary[1] = 0;
		quat.ary[2] = 0;
		quat.ary[3] = 0.70711;
	}

	dev = openRift(0, 0);

	if (dev == NULL) {
		printf("Could not locate Rift\n");
		printf("Be sure you have read/write permission to the proper /dev/hidrawX device\n");
		return;
	}

	setKeepAliveInterval(dev, 1000);

	pthread_t f1_thread;
	dev->runSampleThread = TRUE;
	pthread_create(&f1_thread, NULL, threadFunc, dev);
}

static VECTOR4D_T get_quaternion() {
	//update
	if (dev != NULL) {
		quat.ary[0] = -dev->Q[0];
		quat.ary[1] = dev->Q[1];
		quat.ary[2] = -dev->Q[2];
		quat.ary[3] = dev->Q[3];
	}
	return quat;
}

static VECTOR4D_T get_compass() {
	VECTOR4D_T ret = { };
	return ret;
}

static float get_temperature() {
	return 0;
}

static float get_north() {
	return 0;
}

static void release(void *user_data) {
	free(user_data);
}

static void no_release(void *user_data) {
}

static void create_mpu(void *user_data, MPU_T **mpu) {
	*mpu = lg_mpu;
}

static int command_handler(void *user_data, const char *_buff) {
	return 0;
}

static void event_handler(void *user_data, uint32_t node_id, uint32_t event_id) {
	switch (node_id) {
	case PICAM360_HOST_NODE_ID:
		break;
	default:
		break;
	}
}

static void init_options(void *user_data, json_t *options) {
}

static void save_options(void *user_data, json_t *options) {
}

static char *get_info(void *user_data) {
	return NULL;
}

void create_plugin(PLUGIN_HOST_T *plugin_host, PLUGIN_T **_plugin) {
	init();
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
		plugin->get_info = get_info;
		plugin->user_data = plugin;

		*_plugin = plugin;
	}
	{
		MPU_T *mpu = (MPU_T*) malloc(sizeof(MPU_T));
		memset(mpu, 0, sizeof(MPU_T));
		strcpy(mpu->name, MPU_NAME);
		mpu->release = no_release;
		mpu->get_quaternion = get_quaternion;
		mpu->get_compass = get_compass;
		mpu->get_temperature = get_temperature;
		mpu->get_north = get_north;
		mpu->user_data = mpu;

		lg_mpu = mpu;
	}
	{
		MPU_FACTORY_T *mpu_factory = (MPU_FACTORY_T*) malloc(sizeof(MPU_FACTORY_T));
		memset(mpu_factory, 0, sizeof(MPU_FACTORY_T));
		strcpy(mpu_factory->name, MPU_NAME);
		mpu_factory->release = release;
		mpu_factory->create_mpu = create_mpu;

		lg_plugin_host->add_mpu_factory(mpu_factory);
	}
}
