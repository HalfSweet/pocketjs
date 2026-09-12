/* iOS 6 device services. Like runtime.c, this emits no Objective-C metadata.
 * Camera callbacks own pixels only. JS is entered solely by the frame pump. */
#include "ios-services.h"
#include "quirc/quirc.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef void *Obj;
typedef void *Sel;
typedef struct { float x, y; } Point;
typedef struct { float w, h; } Size;
typedef struct { Point origin; Size size; } Rect;
extern Obj objc_getClass(const char *);
extern Obj objc_allocateClassPair(Obj, const char *, size_t);
extern void objc_registerClassPair(Obj);
extern Obj objc_getProtocol(const char *);
extern int class_addProtocol(Obj, Obj);
extern int class_addMethod(Obj, Sel, void (*)(void), const char *);
extern Sel sel_registerName(const char *);
extern void *objc_msgSend(void);
extern Obj NSHomeDirectory(void);
extern Obj AVMediaTypeVideo;
extern Obj AVCaptureSessionPreset640x480;
extern Obj AVLayerVideoGravityResizeAspectFill;
extern Obj kCVPixelBufferPixelFormatTypeKey;
extern void *CMSampleBufferGetImageBuffer(void *);
extern int CVPixelBufferLockBaseAddress(void *, unsigned long);
extern int CVPixelBufferUnlockBaseAddress(void *, unsigned long);
extern void *CVPixelBufferGetBaseAddress(void *);
extern size_t CVPixelBufferGetWidth(void *);
extern size_t CVPixelBufferGetHeight(void *);
extern size_t CVPixelBufferGetBytesPerRow(void *);
extern void *dispatch_queue_create(const char *, void *);
extern void dispatch_sync_f(void *, void *, void (*)(void *));
extern void dispatch_release(void *);

static Sel sel(const char *s) { return sel_registerName(s); }
static Obj msg(Obj o, const char *s) { return ((Obj (*)(Obj, Sel))objc_msgSend)(o, sel(s)); }
static Obj obj(Obj o, const char *s, Obj p) { return ((Obj (*)(Obj, Sel, Obj))objc_msgSend)(o, sel(s), p); }
static void call(Obj o, const char *s) { if (o) ((void (*)(Obj, Sel))objc_msgSend)(o, sel(s)); }
static void set(Obj o, const char *s, Obj p) { ((void (*)(Obj, Sel, Obj))objc_msgSend)(o, sel(s), p); }
static void number(Obj o, const char *s, long n) { ((void (*)(Obj, Sel, long))objc_msgSend)(o, sel(s), n); }
static void boolean(Obj o, const char *s, int b) { ((void (*)(Obj, Sel, signed char))objc_msgSend)(o, sel(s), (signed char)b); }
static int yes(Obj o, const char *s, Obj p) { return ((signed char (*)(Obj, Sel, Obj))objc_msgSend)(o, sel(s), p); }
static int responds(Obj o, const char *s) { return ((signed char (*)(Obj, Sel, Sel))objc_msgSend)(o, sel("respondsToSelector:"), sel(s)); }
static Obj str(const char *s) { return obj(objc_getClass("NSString"), "stringWithUTF8String:", (Obj)s); }
static const char *utf8(Obj s) { return (const char *)msg(s, "UTF8String"); }
static Obj alloc(const char *name) { return msg(msg(objc_getClass(name), "alloc"), "init"); }
static Rect rect(float x, float y, float w, float h) { Rect r = {{x,y},{w,h}}; return r; }
static void frame(Obj o, Rect r) { ((void (*)(Obj, Sel, Rect))objc_msgSend)(o, sel("setFrame:"), r); }
static Obj view(const char *name, Rect r) { return ((Obj (*)(Obj, Sel, Rect))objc_msgSend)(msg(objc_getClass(name), "alloc"), sel("initWithFrame:"), r); }

static Obj host_window, curtain, scan_view, session, output, preview, delegate;
static void *decode_queue;
static struct quirc *decoder;
static pthread_mutex_t scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static int accepting, scan_pending, result_ready, result_status;
static char result_text[8192];
static size_t result_length;
static unsigned int activity_sequence;
static int app_active = 1;
static double last_decode_ms;
static int decoder_w, decoder_h;

static void publish(int status, const char *text, size_t n) {
  pthread_mutex_lock(&scan_mutex);
  if (scan_pending && !result_ready) {
    if (n >= sizeof result_text) { status = 3; text = "qr_payload_too_large"; n = sizeof "qr_payload_too_large" - 1; }
    result_status = status;
    result_length = n;
    if (result_length) memcpy(result_text, text, result_length);
    result_text[result_length] = 0;
    accepting = 0;
    result_ready = 1;
  }
  pthread_mutex_unlock(&scan_mutex);
}
static void queue_barrier(void *unused) { (void)unused; }
static void stop_camera(void) {
  pthread_mutex_lock(&scan_mutex); accepting = 0; pthread_mutex_unlock(&scan_mutex);
  if (output) ((void (*)(Obj, Sel, Obj, void *))objc_msgSend)(output, sel("setSampleBufferDelegate:queue:"), NULL, NULL);
  call(session, "stopRunning");
  if (decode_queue) dispatch_sync_f(decode_queue, NULL, queue_barrier);
  call(preview, "removeFromSuperlayer"); call(scan_view, "removeFromSuperview");
  call(preview, "release"); call(scan_view, "release"); call(output, "release"); call(session, "release"); call(delegate, "release");
  preview = scan_view = output = session = delegate = NULL;
  if (decode_queue) { dispatch_release(decode_queue); decode_queue = NULL; }
  if (decoder) { quirc_destroy(decoder); decoder = NULL; }
  decoder_w = decoder_h = 0;
}
static void cancelled(Obj self, Sel command) {
  (void)self; (void)command;
  pthread_mutex_lock(&scan_mutex);
  if (scan_pending) {
    accepting = 0; result_status = 2; result_ready = 1; result_length = 0;
    memset(result_text, 0, sizeof result_text);
  }
  pthread_mutex_unlock(&scan_mutex);
  stop_camera();
}

static void captured(Obj self, Sel command, Obj capture_output, void *sample, Obj connection) {
  (void)self; (void)command; (void)capture_output; (void)connection;
  struct timeval tv; gettimeofday(&tv, NULL);
  double ms = (double)tv.tv_sec * 1000 + tv.tv_usec / 1000.0;
  pthread_mutex_lock(&scan_mutex);
  int run = accepting && (ms < last_decode_ms || ms - last_decode_ms >= 125);
  if (run) last_decode_ms = ms;
  pthread_mutex_unlock(&scan_mutex);
  if (!run) return;
  void *pixel = CMSampleBufferGetImageBuffer(sample);
  if (!pixel || CVPixelBufferLockBaseAddress(pixel, 1) != 0) return;
  size_t w = CVPixelBufferGetWidth(pixel), h = CVPixelBufferGetHeight(pixel), stride = CVPixelBufferGetBytesPerRow(pixel);
  const unsigned char *p = CVPixelBufferGetBaseAddress(pixel);
  if (!p || w == 0 || h == 0 || w > 1280 || h > 1280 || stride < w * 4) { CVPixelBufferUnlockBaseAddress(pixel, 1); return; }
  if ((decoder_w != (int)w || decoder_h != (int)h) && quirc_resize(decoder, (int)w, (int)h) < 0) { CVPixelBufferUnlockBaseAddress(pixel, 1); publish(3, "allocation_failed", sizeof "allocation_failed" - 1); return; }
  decoder_w = (int)w; decoder_h = (int)h;
  unsigned char *gray = quirc_begin(decoder, NULL, NULL);
  for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
    const unsigned char *b = p + y * stride + x * 4;
    gray[y*w+x] = (unsigned char)((77U*b[2] + 150U*b[1] + 29U*b[0]) >> 8);
  }
  CVPixelBufferUnlockBaseAddress(pixel, 1);
  quirc_end(decoder);
  /* Scan one provisioning code at a time. Repeated video frames cannot
   * generate repeated imports because publish atomically ends acceptance. */
  for (int i = 0; i < quirc_count(decoder); i++) {
    struct quirc_code code; struct quirc_data data;
    quirc_extract(decoder, i, &code);
    quirc_decode_error_t error = quirc_decode(&code, &data);
    if (error == QUIRC_ERROR_DATA_ECC) { quirc_flip(&code); error = quirc_decode(&code, &data); }
    if (!error) { publish(1, (const char *)data.payload, (size_t)data.payload_len); break; }
  }
}

static Obj label(const char *text, Rect r, float size) {
  Obj l = view("UILabel", r);
  set(l, "setText:", str(text)); set(l, "setTextColor:", msg(objc_getClass("UIColor"), "whiteColor"));
  set(l, "setBackgroundColor:", msg(objc_getClass("UIColor"), "clearColor")); number(l, "setTextAlignment:", 1);
  Obj f = ((Obj (*)(Obj, Sel, float))objc_msgSend)(objc_getClass("UIFont"), sel("systemFontOfSize:"), size);
  set(l, "setFont:", f); return l;
}
static int start_camera(void) {
  if (scan_pending) return 1;
  scan_pending = 1; result_ready = 0; result_length = 0; last_decode_ms = 0;
  if (!app_active || !host_window) { publish(3, "inactive", sizeof "inactive" - 1); return 0; }
  Obj cls = objc_getClass("AVCaptureDevice");
  Obj camera = obj(cls, "defaultDeviceWithMediaType:", AVMediaTypeVideo);
  if (!camera) { publish(3, "camera_unavailable", sizeof "camera_unavailable" - 1); return 0; }
  Obj input_error = NULL;
  Obj input = ((Obj (*)(Obj, Sel, Obj, Obj *))objc_msgSend)(objc_getClass("AVCaptureDeviceInput"), sel("deviceInputWithDevice:error:"), camera, &input_error);
  if (!input) { publish(3, "camera_restricted_or_busy", sizeof "camera_restricted_or_busy" - 1); return 0; }
  session = alloc("AVCaptureSession"); output = alloc("AVCaptureVideoDataOutput"); decoder = quirc_new();
  decode_queue = dispatch_queue_create("dev.pocketjs.qr-decode", NULL);
  if (!session || !output || !decoder || !decode_queue) goto failed;
  if (!yes(session, "canSetSessionPreset:", AVCaptureSessionPreset640x480)) goto failed;
  set(session, "setSessionPreset:", AVCaptureSessionPreset640x480);
  if (!yes(session, "canAddInput:", input) || !yes(session, "canAddOutput:", output)) goto failed;
  set(session, "addInput:", input);
  Obj fmt = ((Obj (*)(Obj, Sel, unsigned int))objc_msgSend)(objc_getClass("NSNumber"), sel("numberWithUnsignedInt:"), 0x42475241U);
  Obj settings = ((Obj (*)(Obj, Sel, Obj, Obj))objc_msgSend)(objc_getClass("NSDictionary"), sel("dictionaryWithObject:forKey:"), fmt, kCVPixelBufferPixelFormatTypeKey);
  set(output, "setVideoSettings:", settings); boolean(output, "setAlwaysDiscardsLateVideoFrames:", 1);
  Obj delegate_class = objc_getClass("PocketQRDelegate");
  if (!delegate_class) {
    delegate_class = objc_allocateClassPair(objc_getClass("NSObject"), "PocketQRDelegate", 0);
    if (!delegate_class) goto failed;
    class_addMethod(delegate_class, sel("captureOutput:didOutputSampleBuffer:fromConnection:"), (void (*)(void))captured, "v@:@@@");
    class_addMethod(delegate_class, sel("cancel"), (void (*)(void))cancelled, "v@:");
    Obj protocol = objc_getProtocol("AVCaptureVideoDataOutputSampleBufferDelegate");
    if (protocol) class_addProtocol(delegate_class, protocol);
    objc_registerClassPair(delegate_class);
  }
  delegate = msg(msg(delegate_class, "alloc"), "init");
  ((void (*)(Obj, Sel, Obj, void *))objc_msgSend)(output, sel("setSampleBufferDelegate:queue:"), delegate, decode_queue);
  set(session, "addOutput:", output);
  scan_view = view("UIView", rect(0, 0, 320, 480));
  set(scan_view, "setBackgroundColor:", msg(objc_getClass("UIColor"), "blackColor"));
  preview = msg(obj(objc_getClass("AVCaptureVideoPreviewLayer"), "layerWithSession:", session), "retain");
  set(preview, "setVideoGravity:", AVLayerVideoGravityResizeAspectFill);
  frame(preview, rect(0, 54, 320, 366));
  Obj connection = msg(preview, "connection");
  if (connection && responds(connection, "setVideoOrientation:")) number(connection, "setVideoOrientation:", 1);
  set(msg(scan_view, "layer"), "addSublayer:", preview);
  Obj title = label("Scan QR code", rect(70, 6, 180, 44), 18);
  set(scan_view, "addSubview:", title); call(title, "release");
  Obj hint = label("Position one code inside the view", rect(8, 427, 304, 42), 14);
  set(scan_view, "addSubview:", hint); call(hint, "release");
  Obj button = ((Obj (*)(Obj, Sel, long))objc_msgSend)(objc_getClass("UIButton"), sel("buttonWithType:"), 0);
  frame(button, rect(4, 4, 72, 44));
  ((void (*)(Obj, Sel, Obj, unsigned long))objc_msgSend)(button, sel("setTitle:forState:"), str("Cancel"), 0);
  ((void (*)(Obj, Sel, Obj, Sel, unsigned long))objc_msgSend)(button, sel("addTarget:action:forControlEvents:"), delegate, sel("cancel"), 1UL << 6);
  set(scan_view, "addSubview:", button); set(host_window, "addSubview:", scan_view);
  pthread_mutex_lock(&scan_mutex); accepting = 1; pthread_mutex_unlock(&scan_mutex);
  call(session, "startRunning");
  if (!((signed char (*)(Obj, Sel))objc_msgSend)(session, sel("isRunning"))) goto failed;
  return 0;
failed:
  publish(3, "camera_start_failed", sizeof "camera_start_failed" - 1); stop_camera(); return 0;
}

static JSValue json_line(JSContext *c, JSValue value) {
  JSValue result = JS_JSONStringify(c, value, JS_UNDEFINED, JS_UNDEFINED); JS_FreeValue(c, value); return result;
}
static JSValue clipboard_result(JSContext *c, int ok, const char *code, Obj text) {
  JSValue o = JS_NewObject(c); JS_SetPropertyStr(c, o, "ok", JS_NewBool(c, ok));
  if (!ok) JS_SetPropertyStr(c, o, "code", JS_NewString(c, code));
  if (text) JS_SetPropertyStr(c, o, "text", JS_NewString(c, utf8(text)));
  return json_line(c, o);
}
static JSValue device_op(JSContext *c, JSValueConst self, int argc, JSValueConst *argv, int op) {
  (void)self;
  if (op == 0) return JS_NewInt32(c, start_camera());
  if (op == 1) {
    if (scan_pending && session) {
      int running = ((signed char (*)(Obj, Sel))objc_msgSend)(session, sel("isRunning"));
      int interrupted = responds(session, "isInterrupted") && ((signed char (*)(Obj, Sel))objc_msgSend)(session, sel("isInterrupted"));
      if (!running || interrupted) publish(3, "camera_interrupted", sizeof "camera_interrupted" - 1);
    }
    pthread_mutex_lock(&scan_mutex); int ready = result_ready; pthread_mutex_unlock(&scan_mutex);
    if (!ready) return JS_UNDEFINED;
    stop_camera();
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "status", JS_NewString(c, result_status == 1 ? "decoded" : result_status == 2 ? "cancelled" : "error"));
    if (result_status == 1) JS_SetPropertyStr(c, o, "text", JS_NewStringLen(c, result_text, result_length));
    if (result_status == 3) JS_SetPropertyStr(c, o, "code", JS_NewStringLen(c, result_text, result_length));
    memset(result_text, 0, sizeof result_text); result_length = 0; result_ready = 0; scan_pending = 0;
    return json_line(c, o);
  }
  if (op == 2) { cancelled(NULL, NULL); return JS_UNDEFINED; }
  if (op == 5) return JS_NewUint32(c, activity_sequence);
  if (!app_active) return clipboard_result(c, 0, "inactive", NULL);
  Obj pasteboard = msg(objc_getClass("UIPasteboard"), "generalPasteboard");
  if (!pasteboard) return clipboard_result(c, 0, "clipboard_unavailable", NULL);
  if (op == 3) { Obj value = msg(pasteboard, "string"); return clipboard_result(c, 1, NULL, value ? value : str("")); }
  if (op == 4) {
    size_t n = 0; const char *text = argc ? JS_ToCStringLen(c, &n, argv[0]) : NULL;
    if (!text) return clipboard_result(c, 0, "invalid_text", NULL);
    Obj value = ((Obj (*)(Obj, Sel, const void *, unsigned long, unsigned long))objc_msgSend)(msg(objc_getClass("NSString"), "alloc"), sel("initWithBytes:length:encoding:"), text, (unsigned long)n, 4UL);
    JS_FreeCString(c, text);
    if (!value) return clipboard_result(c, 0, "invalid_text", NULL);
    set(pasteboard, "setString:", value);
    int ok = yes(value, "isEqualToString:", msg(pasteboard, "string")); call(value, "release");
    return clipboard_result(c, ok, "clipboard_write_failed", NULL);
  }
  return JS_UNDEFINED;
}

void pocket_ios_services_window(void *window) { host_window = window; }
void pocket_ios_services_active(int active) {
  if (active == app_active) return;
  app_active = active; activity_sequence++;
  if (!active) {
    /* This UIKit cover is synchronous, before the system takes a snapshot. */
    cancelled(NULL, NULL);
    if (!curtain && host_window) {
      curtain = view("UIView", rect(0, 0, 320, 480));
      set(curtain, "setBackgroundColor:", msg(objc_getClass("UIColor"), "blackColor"));
      set(host_window, "addSubview:", curtain);
    }
  }
  /* Remove only after the first resumed JS frame has discarded drafts. */
}
void pocket_ios_services_presented(void) {
  if (app_active && curtain) { call(curtain, "removeFromSuperview"); call(curtain, "release"); curtain = NULL; }
}
void pocket_ios_services_shutdown(void) {
  cancelled(NULL, NULL); scan_pending = result_ready = 0; memset(result_text, 0, sizeof result_text);
}
int pocket_ios_services_mount(JSContext *c, JSValueConst global) {
  const char *home = utf8(NSHomeDirectory()); char root[1024];
  if (!home || snprintf(root, sizeof root, "%s/Library/PocketJS/data", home) >= (int)sizeof root || !pocket_ios_fs_mount(c, global, root)) return 0;
  JSValue ns = JS_NewObject(c);
  const char *names[] = {"scanStart", "scanPoll", "scanCancel", "clipboardRead", "clipboardWrite", "activity"};
  for (int i = 0; i < 6; i++) JS_SetPropertyStr(c, ns, names[i], JS_NewCFunctionMagic(c, device_op, names[i], i == 4 ? 1 : 0, JS_CFUNC_generic_magic, i));
  return JS_SetPropertyStr(c, global, "device", ns) >= 0;
}
