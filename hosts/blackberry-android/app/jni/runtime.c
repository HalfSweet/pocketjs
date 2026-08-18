#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pocket_runtime.h"

#define LOG_TAG "PocketJSClassic"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* The logical viewport comes from the resolved build plan (blackberry-android.ts);
 * the defaults match the private blackberry-android-dev profile. */
#ifndef POCKET_LOGICAL_WIDTH
#define POCKET_LOGICAL_WIDTH 360
#endif
#ifndef POCKET_LOGICAL_HEIGHT
#define POCKET_LOGICAL_HEIGHT 360
#endif

#define BTN_START 0x0008U
#define BTN_UP 0x0010U
#define BTN_RIGHT 0x0020U
#define BTN_DOWN 0x0040U
#define BTN_LEFT 0x0080U
#define BTN_TRIANGLE 0x1000U
#define BTN_CIRCLE 0x2000U

#define KEYCODE_BACK 4
#define KEYCODE_DPAD_UP 19
#define KEYCODE_DPAD_DOWN 20
#define KEYCODE_DPAD_LEFT 21
#define KEYCODE_DPAD_RIGHT 22
#define KEYCODE_DPAD_CENTER 23
#define KEYCODE_SPACE 62
#define KEYCODE_ENTER 66
#define KEYCODE_MENU 82
#define KEYCODE_NUMPAD_ENTER 160

#define ACTION_DOWN 0
#define ACTION_UP 1
#define ACTION_CANCEL 3
#define ACTION_POINTER_DOWN 5
#define ACTION_POINTER_UP 6
#define BUTTON_PRIMARY 1

typedef struct {
  int id;
  int down;
  int latched;
  float physical_x;
  float physical_y;
} TouchState;

static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t held_buttons;
static uint32_t pressed_buttons;
static float relative_x;
static float relative_y;
static int relative_primary_down;
static TouchState touch_state = {-1, 0, 0, 0.0f, 0.0f};
static int surface_width = 720;
static int surface_height = 720;

static uint8_t *guest_js;
static size_t guest_js_length;
static uint8_t *guest_pack;
static size_t guest_pack_length;
static int runtime_booted;
static int gl_initialized;
static char android_error[512];

/* The Rust core is panic=abort. Satisfy old Android loaders without pulling in
 * an unwinder; reaching this symbol would already be a fatal runtime defect. */
__attribute__((noreturn)) void rust_eh_personality(void)
{
  abort();
}

static void set_android_error(const char *message)
{
  size_t length = message == NULL ? 0 : strlen(message);
  if (length >= sizeof(android_error)) length = sizeof(android_error) - 1;
  if (length > 0) memcpy(android_error, message, length);
  android_error[length] = '\0';
  LOGE("%s", android_error);
}

static uint8_t *copy_java_bytes(
  JNIEnv *env,
  jbyteArray source,
  size_t *length
)
{
  if (source == NULL) return NULL;
  jsize source_length = (*env)->GetArrayLength(env, source);
  if (source_length <= 0) return NULL;
  uint8_t *bytes = (uint8_t *)malloc((size_t)source_length);
  if (bytes == NULL) return NULL;
  (*env)->GetByteArrayRegion(env, source, 0, source_length, (jbyte *)bytes);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
    free(bytes);
    return NULL;
  }
  *length = (size_t)source_length;
  return bytes;
}

static uint32_t button_for_key(int key_code)
{
  switch (key_code) {
    case KEYCODE_DPAD_UP: return BTN_UP;
    case KEYCODE_DPAD_RIGHT: return BTN_RIGHT;
    case KEYCODE_DPAD_DOWN: return BTN_DOWN;
    case KEYCODE_DPAD_LEFT: return BTN_LEFT;
    case KEYCODE_DPAD_CENTER:
    case KEYCODE_ENTER:
    case KEYCODE_NUMPAD_ENTER:
      return BTN_CIRCLE;
    case KEYCODE_SPACE: return BTN_START;
    case KEYCODE_MENU: return BTN_TRIANGLE;
    default: return 0;
  }
}

static void pulse_relative_axis(float delta_x, float delta_y)
{
  const float threshold = 0.35f;
  relative_x += delta_x;
  relative_y += delta_y;
  if (relative_x <= -threshold) {
    pressed_buttons |= BTN_LEFT;
    relative_x += threshold;
  } else if (relative_x >= threshold) {
    pressed_buttons |= BTN_RIGHT;
    relative_x -= threshold;
  }
  if (relative_y <= -threshold) {
    pressed_buttons |= BTN_UP;
    relative_y += threshold;
  } else if (relative_y >= threshold) {
    pressed_buttons |= BTN_DOWN;
    relative_y -= threshold;
  }
}

JNIEXPORT jstring JNICALL
Java_dev_pocketstack_blackberry_PocketActivity_nativeSurfaceCreated(
  JNIEnv *env,
  jclass owner,
  jbyteArray guest_java_script,
  jbyteArray guest_asset_pack
)
{
  (void)owner;
  android_error[0] = '\0';
  if (runtime_booted) {
    pocket_runtime_gl_reset();
    gl_initialized = pocket_runtime_gl_initialize();
    if (!gl_initialized) set_android_error("GLES2 backend reinitialization failed");
    return (*env)->NewStringUTF(env, gl_initialized ? "ok" : android_error);
  }

  uint8_t *new_guest_js = copy_java_bytes(
    env,
    guest_java_script,
    &guest_js_length
  );
  uint8_t *new_guest_pack = copy_java_bytes(
    env,
    guest_asset_pack,
    &guest_pack_length
  );
  if (new_guest_js == NULL || new_guest_pack == NULL) {
    free(new_guest_js);
    free(new_guest_pack);
    set_android_error("APK assets/app.js or assets/app.pak could not be copied");
    return (*env)->NewStringUTF(env, android_error);
  }
  free(guest_js);
  free(guest_pack);
  guest_js = new_guest_js;
  guest_pack = new_guest_pack;

  if (!pocket_runtime_boot(
        (const char *)guest_js,
        guest_js_length,
        guest_pack,
        guest_pack_length,
        POCKET_LOGICAL_WIDTH,
        POCKET_LOGICAL_HEIGHT
      )) {
    set_android_error(pocket_runtime_error());
    return (*env)->NewStringUTF(env, android_error);
  }
  runtime_booted = 1;
  gl_initialized = pocket_runtime_gl_initialize();
  if (!gl_initialized) {
    set_android_error("PocketJS GLES2 backend initialization failed");
  }
  return (*env)->NewStringUTF(env, gl_initialized ? "ok" : android_error);
}

JNIEXPORT void JNICALL
Java_dev_pocketstack_blackberry_PocketActivity_nativeSurfaceChanged(
  JNIEnv *env,
  jclass owner,
  jint width,
  jint height
)
{
  (void)env;
  (void)owner;
  pthread_mutex_lock(&input_mutex);
  surface_width = width > 0 ? width : 1;
  surface_height = height > 0 ? height : 1;
  pthread_mutex_unlock(&input_mutex);
}

JNIEXPORT jboolean JNICALL
Java_dev_pocketstack_blackberry_PocketActivity_nativeFrame(
  JNIEnv *env,
  jclass owner
)
{
  (void)env;
  (void)owner;
  if (!runtime_booted || !gl_initialized) return JNI_FALSE;

  uint32_t buttons;
  int touch_down;
  float physical_x;
  float physical_y;
  int width;
  int height;
  pthread_mutex_lock(&input_mutex);
  buttons = held_buttons | pressed_buttons;
  pressed_buttons = 0;
  touch_down = touch_state.down || touch_state.latched;
  physical_x = touch_state.physical_x;
  physical_y = touch_state.physical_y;
  touch_state.latched = 0;
  if (!touch_state.down) touch_state.id = -1;
  width = surface_width;
  height = surface_height;
  pthread_mutex_unlock(&input_mutex);

  int logical_x = (int)(physical_x * POCKET_LOGICAL_WIDTH / width);
  int logical_y = (int)(physical_y * POCKET_LOGICAL_HEIGHT / height);
  int hit = touch_down
    ? pocket_runtime_hit_test_bounds((float)logical_x, (float)logical_y)
    : 0;
  if (!pocket_runtime_frame_input_ticks(
        buttons,
        touch_down,
        logical_x,
        logical_y,
        hit,
        1
      )) {
    set_android_error(pocket_runtime_error());
    return JNI_FALSE;
  }
  if (!pocket_runtime_gl_render(width, height)) {
    set_android_error("PocketJS GLES2 frame submission failed");
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

JNIEXPORT jstring JNICALL
Java_dev_pocketstack_blackberry_PocketActivity_nativeError(JNIEnv *env, jclass owner)
{
  (void)owner;
  const char *message = android_error[0] != '\0'
    ? android_error
    : pocket_runtime_error();
  return (*env)->NewStringUTF(env, message == NULL ? "unknown error" : message);
}

JNIEXPORT void JNICALL
Java_dev_pocketstack_blackberry_PocketActivity_nativeKey(
  JNIEnv *env,
  jclass owner,
  jint action,
  jint key_code,
  jint scan_code,
  jint unicode,
  jint repeat
)
{
  (void)env;
  (void)owner;
  (void)scan_code;
  (void)unicode;
  uint32_t button = button_for_key(key_code);
  if (button == 0 || key_code == KEYCODE_BACK) return;
  pthread_mutex_lock(&input_mutex);
  if (action == ACTION_DOWN) {
    held_buttons |= button;
    if (repeat == 0) pressed_buttons |= button;
  } else if (action == ACTION_UP) {
    held_buttons &= ~button;
  }
  pthread_mutex_unlock(&input_mutex);
}

JNIEXPORT void JNICALL
Java_dev_pocketstack_blackberry_PocketActivity_nativeTouch(
  JNIEnv *env,
  jclass owner,
  jint action,
  jint pointer_id,
  jfloat x,
  jfloat y
)
{
  (void)env;
  (void)owner;
  pthread_mutex_lock(&input_mutex);
  if (action == ACTION_DOWN || action == ACTION_POINTER_DOWN) {
    if (touch_state.id < 0) {
      touch_state.id = pointer_id;
      touch_state.down = 1;
      touch_state.latched = 1;
      touch_state.physical_x = x;
      touch_state.physical_y = y;
    }
  } else if (action == ACTION_CANCEL) {
    touch_state.id = -1;
    touch_state.down = 0;
    touch_state.latched = 0;
  } else if (touch_state.id == pointer_id) {
    touch_state.physical_x = x;
    touch_state.physical_y = y;
    if (action == ACTION_UP || action == ACTION_POINTER_UP) {
      touch_state.down = 0;
    }
  }
  pthread_mutex_unlock(&input_mutex);
}

JNIEXPORT void JNICALL
Java_dev_pocketstack_blackberry_PocketActivity_nativeRelative(
  JNIEnv *env,
  jclass owner,
  jfloat delta_x,
  jfloat delta_y,
  jint action,
  jint button_state
)
{
  (void)env;
  (void)owner;
  pthread_mutex_lock(&input_mutex);
  pulse_relative_axis(delta_x, delta_y);
  int primary = (button_state & BUTTON_PRIMARY) != 0 || action == ACTION_DOWN;
  if (primary && !relative_primary_down) {
    held_buttons |= BTN_CIRCLE;
    pressed_buttons |= BTN_CIRCLE;
  } else if (!primary && relative_primary_down) {
    held_buttons &= ~BTN_CIRCLE;
  }
  if (action == ACTION_UP || action == ACTION_CANCEL) {
    held_buttons &= ~BTN_CIRCLE;
    primary = 0;
  }
  relative_primary_down = primary;
  pthread_mutex_unlock(&input_mutex);
}
