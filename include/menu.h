#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define MENU_NAME_MAX_LENGTH 256

enum MENU_EVENT {
	MENU_EVENT_NONE,
	MENU_EVENT_ACTIVATED,
	MENU_EVENT_DEACTIVATED,
	MENU_EVENT_SELECTED,
	MENU_EVENT_DESELECTED,
	MENU_EVENT_BEFORE_DELETE,
};
enum MENU_OPERATE {
	MENU_OPERATE_NONE,
	MENU_OPERATE_ACTIVE_BACK,
	MENU_OPERATE_ACTIVE_NEXT,
	MENU_OPERATE_SELECT,
	MENU_OPERATE_DESELECT,
};

struct _MENU_T;
typedef void (*MENU_CALLBACK)(struct _MENU_T *menu, enum MENU_EVENT event);
typedef struct _MENU_T {
	char name[MENU_NAME_MAX_LENGTH];
	bool activated;
	bool selected;
	bool marked;
	MENU_CALLBACK callback;
	struct _MENU_T *parent;
	struct _MENU_T *submenu[256];
	void *user_data;
} MENU_T;

void init_menu(uint32_t font_size);
void deinit_menu();
void menu_redraw(MENU_T *root, char *_status, uint32_t screen_width,
		uint32_t screen_height, uint32_t frame_width, uint32_t frame_height,
		bool stereo);
MENU_T *menu_new(char *name, MENU_CALLBACK callback, void *user_data);
void menu_delete(MENU_T **menu);
MENU_T *menu_add_submenu(MENU_T *parent, MENU_T *child, int idx);
MENU_T *menu_get_submenu(MENU_T *parent, char *name, bool create_new);
void menu_operate(MENU_T *root, enum MENU_OPERATE operate);
