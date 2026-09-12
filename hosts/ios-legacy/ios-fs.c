/* The data.fs nine-op contract for the C QuickJS legacy host. No Apple
 * framework dependencies; the same implementation is exercised on macOS. */
#include "ios-services.h"
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IO_MAX 65536
#define PATH_MAXIMUM 1024
static char data_root[PATH_MAXIMUM];
static char fs_error[96];
static unsigned int temp_sequence;
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void error_set(const char *s) { snprintf(fs_error, sizeof fs_error, "%s", s); }
static JSValue line(JSContext *c, JSValue object) {
  JSValue out = JS_JSONStringify(c, object, JS_UNDEFINED, JS_UNDEFINED);
  JS_FreeValue(c, object); return out;
}
static JSValue error_line(JSContext *c, const char *s) {
  error_set(s); JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "error", JS_NewString(c, s)); return line(c, o);
}
static int utf8_valid(const unsigned char *p, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned int a = p[i++], v; int k;
    if (a < 0x80) continue;
    if (a >= 0xc2 && a <= 0xdf) { v = a & 31; k = 1; }
    else if (a >= 0xe0 && a <= 0xef) { v = a & 15; k = 2; }
    else if (a >= 0xf0 && a <= 0xf4) { v = a & 7; k = 3; }
    else return 0;
    int count = k;
    while (k--) { if (i >= n || (p[i] & 0xc0) != 0x80) return 0; v = (v << 6) | (p[i++] & 63); }
    if ((count == 2 && v < 0x800) || (count == 3 && v < 0x10000) ||
        (v >= 0xd800 && v <= 0xdfff) || v > 0x10ffff) return 0;
  }
  return 1;
}

/* Every segment is lstat-checked, including the final name. Guest paths
 * cannot escape the root, and host-created symlinks are never followed. */
static int resolve_path(const char *s, size_t n, char *out, int allow_root, int parents) {
  if (n > 160 || (!n && !allow_root) || memchr(s, 0, n) || !utf8_valid((const unsigned char *)s, n)) goto invalid;
  if (!n) { snprintf(out, PATH_MAXIMUM, "%s", data_root); return 1; }
  size_t start = 0; unsigned int depth = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i < n && ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127)) goto invalid;
    if (i != n && s[i] != '/') continue;
    size_t len = i - start;
    if (!len || len > 64 || ++depth > 8 || (len == 1 && s[start] == '.') ||
        (len == 2 && s[start] == '.' && s[start+1] == '.')) goto invalid;
    int written = snprintf(out, PATH_MAXIMUM, "%s/%.*s", data_root, (int)i, s);
    if (written < 0 || written >= PATH_MAXIMUM) goto invalid;
    struct stat st;
    if (lstat(out, &st) == 0) {
      if (S_ISLNK(st.st_mode)) { error_set("symlink refused"); return 0; }
      if (i < n && !S_ISDIR(st.st_mode)) { error_set("not a directory"); return 0; }
    } else if (errno != ENOENT) { error_set("path inaccessible"); return 0; }
    else if (parents && i < n && mkdir(out, 0700) != 0 && errno != EEXIST) { error_set("mkdir failed"); return 0; }
    start = i + 1;
  }
  return 1;
invalid: error_set("invalid path"); return 0;
}

static char *encode64(const unsigned char *in, size_t n) {
  char *s = malloc(4 * ((n+2)/3) + 1); if (!s) return NULL;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    unsigned int v = (unsigned int)in[i] << 16;
    if (i+1 < n) v |= (unsigned int)in[i+1] << 8;
    if (i+2 < n) v |= in[i+2];
    s[o++] = b64[v >> 18]; s[o++] = b64[(v >> 12) & 63];
    s[o++] = i+1 < n ? b64[(v >> 6) & 63] : '='; s[o++] = i+2 < n ? b64[v & 63] : '=';
  }
  s[o] = 0; return s;
}
static unsigned char *decode64(const char *s, size_t n, size_t *length) {
  if (n % 4 || n > 4 * ((IO_MAX+2)/3)) return NULL;
  unsigned char *out = malloc(n / 4 * 3 + 1); if (!out) return NULL;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 4) {
    unsigned int v = 0; int pad = 0;
    for (int j = 0; j < 4; j++) {
      const char *p = s[i+j] ? strchr(b64, s[i+j]) : NULL;
      if (s[i+j] == '=' && j >= 2 && i+4 == n) { pad++; v <<= 6; }
      else if (!p || pad) { free(out); return NULL; }
      else v = (v << 6) | (unsigned int)(p-b64);
    }
    if ((pad == 1 && (v & 255)) || (pad == 2 && (v & 65535))) { free(out); return NULL; }
    out[o++] = (unsigned char)(v >> 16);
    if (pad < 2) out[o++] = (unsigned char)(v >> 8);
    if (!pad) out[o++] = (unsigned char)v;
  }
  *length = o; return out;
}
static int write_bytes(int fd, const unsigned char *p, size_t n) {
  while (n) { ssize_t k = write(fd, p, n); if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return 0; p += k; n -= (size_t)k; } return 1;
}
static int walk(const char *path, int remove_it, double *bytes) {
  struct stat st; if (lstat(path, &st) != 0) return errno == ENOENT;
  if (S_ISLNK(st.st_mode)) return !remove_it;
  if (!S_ISDIR(st.st_mode)) { *bytes += (double)st.st_size; return !remove_it || unlink(path) == 0; }
  DIR *d = opendir(path); if (!d) return 0; int ok = 1; struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child[PATH_MAXIMUM];
    if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child || !walk(child, remove_it, bytes)) { ok = 0; break; }
  }
  closedir(d); return ok && (!remove_it || rmdir(path) == 0);
}
static int compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int integer_arg(JSContext *c, JSValueConst v, double *d) {
  if (JS_ToFloat64(c, d, v) < 0) return 0;
  return *d >= 0 && *d <= 9007199254740991.0 && (double)(unsigned long long)*d == *d;
}

static JSValue fs_op(JSContext *c, JSValueConst self, int argc, JSValueConst *argv, int op) {
  (void)self;
  if (op == 9) return JS_NewString(c, fs_error);
  fs_error[0] = 0;
  if (op == 8) {
    double bytes = 0; if (!walk(data_root, 0, &bytes)) return error_line(c, "usage failed");
    JSValue o = JS_NewObject(c); JS_SetPropertyStr(c, o, "usedBytes", JS_NewFloat64(c, bytes));
    JS_SetPropertyStr(c, o, "quotaBytes", JS_NewInt32(c, 0)); return line(c, o);
  }
  char path[PATH_MAXIMUM]; size_t n = 0;
  const char *s = argc ? JS_ToCStringLen(c, &n, argv[0]) : NULL;
  int valid = s && resolve_path(s, n, path, op == 4 || op == 5, op == 2 || op == 6);
  if (s) JS_FreeCString(c, s);
  if (!valid) { if (!fs_error[0]) error_set("invalid path"); goto failed; }
  if (op == 1) {
    double offset, max;
    if (argc < 3 || !integer_arg(c, argv[1], &offset) || !integer_arg(c, argv[2], &max) || max < 1 || max > IO_MAX) { error_set("invalid read range"); goto failed; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW); struct stat st;
    if (fd < 0) { error_set("file unavailable"); goto failed; }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || lseek(fd, (off_t)offset, SEEK_SET) < 0) { close(fd); error_set("read failed"); goto failed; }
    unsigned char *buf = malloc((size_t)max); if (!buf) { close(fd); error_set("allocation failed"); goto failed; }
    ssize_t count; do { count = read(fd, buf, (size_t)max); } while (count < 0 && errno == EINTR);
    close(fd); if (count < 0) { free(buf); error_set("read failed"); goto failed; }
    char *encoded = encode64(buf, (size_t)count); free(buf);
    if (!encoded) { error_set("allocation failed"); goto failed; }
    JSValue o = JS_NewObject(c), data = JS_NewObject(c);
    JS_SetPropertyStr(c, data, "$b", JS_NewString(c, encoded)); free(encoded);
    JS_SetPropertyStr(c, o, "data", data); JS_SetPropertyStr(c, o, "size", JS_NewFloat64(c, (double)st.st_size));
    JS_SetPropertyStr(c, o, "eof", JS_NewBool(c, offset + (double)count >= (double)st.st_size)); return line(c, o);
  }
  if (op == 2) {
    int32_t mode = -1;
    if (argc < 3 || JS_ToInt32(c, &mode, argv[2]) < 0 || (mode != 0 && mode != 1)) { error_set("invalid write mode"); goto failed; }
    const char *json = JS_ToCStringLen(c, &n, argv[1]); if (!json) goto failed;
    JSValue data = JS_ParseJSON(c, json, n, "fs payload"); JS_FreeCString(c, json);
    if (JS_IsException(data)) { JS_FreeValue(c, JS_GetException(c)); error_set("malformed payload"); goto failed; }
    const unsigned char *bytes = NULL; unsigned char *allocated = NULL; const char *text = NULL; size_t count = 0;
    if (JS_IsString(data)) { text = JS_ToCStringLen(c, &count, data); bytes = (const unsigned char *)text; }
    else if (JS_IsObject(data)) { JSValue v = JS_GetPropertyStr(c, data, "$b");
      if (JS_IsString(v)) { const char *b = JS_ToCStringLen(c, &n, v); if (b) { allocated = decode64(b, n, &count); JS_FreeCString(c, b); } }
      JS_FreeValue(c, v); bytes = allocated;
    }
    int ok = 0;
    if (!bytes || count > IO_MAX || (text && !utf8_valid(bytes, count))) error_set("invalid or oversized payload");
    else {
      char tmp[PATH_MAXIMUM];
      snprintf(tmp, sizeof tmp, "%s/../atomic-%u.tmp", data_root, ++temp_sequence);
      int fd = open(mode == 0 ? tmp : path, O_WRONLY | O_CREAT | O_NOFOLLOW | (mode == 0 ? O_EXCL : O_APPEND), 0600);
      if (fd >= 0) { ok = write_bytes(fd, bytes, count); if (ok) ok = fsync(fd) == 0; if (close(fd) != 0) ok = 0;
        if (mode == 0) { if (ok) ok = rename(tmp, path) == 0; if (!ok) unlink(tmp); }
      }
      if (!ok) error_set("write failed");
    }
    if (text) JS_FreeCString(c, text); free(allocated); JS_FreeValue(c, data);
    return JS_NewInt32(c, ok ? 0 : 1);
  }
  if (op == 3) {
    int recursive = argc > 1 && JS_ToBool(c, argv[1]); double bytes = 0;
    struct stat st;
    if (lstat(path, &st) != 0) { if (errno == ENOENT) return JS_NewInt32(c, 0); error_set("remove failed"); goto failed; }
    int ok = recursive ? walk(path, 1, &bytes) : (S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) == 0;
    if (!ok) error_set("remove failed"); return JS_NewInt32(c, ok ? 0 : 1);
  }
  if (op == 4) {
    double offset;
    if (argc < 2 || !integer_arg(c, argv[1], &offset)) { error_set("invalid list offset"); goto failed; }
    DIR *d = opendir(path); if (!d) { error_set("directory unavailable"); goto failed; }
    char **names = NULL; size_t count = 0; int ok = 1; struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char child[PATH_MAXIMUM]; struct stat st;
      if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) continue;
      if (lstat(child, &st) != 0 || S_ISLNK(st.st_mode)) continue;
      char **next = realloc(names, (count+1) * sizeof *names); if (!next) { ok = 0; break; }
      names = next; names[count] = strdup(e->d_name); if (!names[count]) { ok = 0; break; } count++;
    }
    closedir(d); if (count) qsort(names, count, sizeof *names, compare_names);
    JSValue entries = JS_NewArray(c); size_t start = offset > (double)count ? count : (size_t)offset;
    size_t end = start + 256 < count ? start + 256 : count;
    for (size_t i = start; ok && i < end; i++) {
      char child[PATH_MAXIMUM]; struct stat st; snprintf(child, sizeof child, "%s/%s", path, names[i]);
      if (lstat(child, &st) != 0) { ok = 0; break; }
      JSValue e2 = JS_NewObject(c); JS_SetPropertyStr(c, e2, "name", JS_NewString(c, names[i]));
      JS_SetPropertyStr(c, e2, "kind", JS_NewString(c, S_ISDIR(st.st_mode) ? "dir" : "file"));
      JS_SetPropertyStr(c, e2, "size", JS_NewFloat64(c, S_ISDIR(st.st_mode) ? 0 : (double)st.st_size));
      JS_SetPropertyUint32(c, entries, (uint32_t)(i-start), e2);
    }
    for (size_t i = 0; i < count; i++) free(names[i]); free(names);
    if (!ok) { JS_FreeValue(c, entries); error_set("list failed"); goto failed; }
    JSValue o = JS_NewObject(c); JS_SetPropertyStr(c, o, "entries", entries);
    JS_SetPropertyStr(c, o, "eof", JS_NewBool(c, end == count)); return line(c, o);
  }
  if (op == 5) {
    struct stat st; if (lstat(path, &st) != 0) { error_set("not found"); goto failed; }
    JSValue o = JS_NewObject(c); JS_SetPropertyStr(c, o, "kind", JS_NewString(c, S_ISDIR(st.st_mode) ? "dir" : "file"));
    JS_SetPropertyStr(c, o, "size", JS_NewFloat64(c, S_ISDIR(st.st_mode) ? 0 : (double)st.st_size)); return line(c, o);
  }
  if (op == 6) {
    struct stat st; if (mkdir(path, 0700) == 0 || (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))) return JS_NewInt32(c, 0);
    error_set("mkdir failed"); goto failed;
  }
  if (op == 7) {
    char dest[PATH_MAXIMUM]; const char *to = argc > 1 ? JS_ToCStringLen(c, &n, argv[1]) : NULL;
    int ok = to && resolve_path(to, n, dest, 0, 1); if (to) JS_FreeCString(c, to);
    if (ok && rename(path, dest) == 0) return JS_NewInt32(c, 0);
    error_set("rename failed"); goto failed;
  }
failed:
  if (!fs_error[0]) error_set("file operation failed");
  if (op == 1 || op == 4 || op == 5) { char saved[96]; snprintf(saved, sizeof saved, "%s", fs_error); return error_line(c, saved); }
  return JS_NewInt32(c, 1);
}

int pocket_ios_fs_mount(JSContext *c, JSValueConst global, const char *root) {
  if (!root || strlen(root) + 180 >= sizeof data_root) return 0;
  snprintf(data_root, sizeof data_root, "%s", root);
  for (char *p = data_root + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(data_root, 0700) != 0 && errno != EEXIST) return 0; *p = '/'; }
  if (mkdir(data_root, 0700) != 0 && errno != EEXIST) return 0;
  /* Atomic temp files belong to the module, never the guest data tree. */
  char parent[PATH_MAXIMUM]; snprintf(parent, sizeof parent, "%s/..", data_root);
  DIR *d = opendir(parent); struct dirent *e;
  if (d) {
    while ((e = readdir(d))) {
      if (!strncmp(e->d_name, "atomic-", 7)) {
        char tmp[PATH_MAXIMUM]; if (snprintf(tmp, sizeof tmp, "%s/%s", parent, e->d_name) < (int)sizeof tmp) unlink(tmp);
      }
    }
    closedir(d);
  }
  const char *names[] = { "read", "write", "remove", "list", "stat", "mkdir", "rename", "usage", "lastError" };
  const int arities[] = { 3, 3, 2, 2, 1, 1, 2, 0, 0 };
  JSValue ns = JS_NewObject(c);
  for (int i = 0; i < 9; i++) JS_SetPropertyStr(c, ns, names[i], JS_NewCFunctionMagic(c, fs_op, names[i], arities[i], JS_CFUNC_generic_magic, i+1));
  return JS_SetPropertyStr(c, global, "fs", ns) >= 0;
}
