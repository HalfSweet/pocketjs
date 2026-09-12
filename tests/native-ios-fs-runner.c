#include "quickjs.h"
#include "ios-services.h"
#include <stdio.h>
#include <stdlib.h>

/* Runs a bundled JS acceptance script against the actual C data.fs module
 * and pinned QuickJS, with the same JS stack ceiling as the iOS host. */
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  FILE *file = fopen(argv[1], "rb"); if (!file) return 2;
  fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
  if (size < 0 || size > 8 * 1024 * 1024) return 2;
  char *source = malloc((size_t)size + 1); if (!source) return 2;
  if (fread(source, 1, (size_t)size, file) != (size_t)size) return 2;
  fclose(file); source[size] = 0;
  JSRuntime *runtime = JS_NewRuntime(); JS_SetMaxStackSize(runtime, 256 * 1024);
  JSContext *context = JS_NewContext(runtime); JSValue global = JS_GetGlobalObject(context);
  if (!pocket_ios_fs_mount(context, global, argv[2])) return 2;
  JSValue result = JS_Eval(context, source, (size_t)size, "native-test.js", JS_EVAL_TYPE_GLOBAL);
  int failed = JS_IsException(result);
  if (failed) {
    JSValue error = JS_GetException(context);
    const char *message = JS_ToCString(context, error);
    fprintf(stderr, "native guest: %s\n", message ? message : "exception");
    JS_FreeCString(context, message); JS_FreeValue(context, error);
  } else puts("PASS: native QuickJS and C file storage acceptance");
  JS_FreeValue(context, result); JS_FreeValue(context, global);
  JS_FreeContext(context); JS_FreeRuntime(runtime); free(source);
  return failed ? 1 : 0;
}
