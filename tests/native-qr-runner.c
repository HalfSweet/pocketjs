#include "quirc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  FILE *fp = fopen(argv[1], "rb"); if (!fp) return 2;
  char line[64]; int w = 0, h = 0;
  if (!fgets(line, sizeof line, fp) || strcmp(line, "P5\n") || !fgets(line, sizeof line, fp) || sscanf(line, "%d %d", &w, &h) != 2 ||
      !fgets(line, sizeof line, fp) || strcmp(line,"255\n") || w <= 0 || h <= 0 || w > 1280 || h > 1280) return 2;
  struct quirc *decoder = quirc_new(); if (!decoder || quirc_resize(decoder, w, h) < 0) return 2;
  unsigned char *pixels = quirc_begin(decoder, NULL, NULL);
  if (fread(pixels, 1, (size_t)w*h, fp) != (size_t)w*h) return 2;
  fclose(fp); quirc_end(decoder);
  int matched = 0, decoded = 0;
  for (int i = 0; i < quirc_count(decoder); i++) {
    struct quirc_code code; struct quirc_data data;
    quirc_extract(decoder, i, &code); quirc_decode_error_t err = quirc_decode(&code, &data);
    if (err == QUIRC_ERROR_DATA_ECC) { quirc_flip(&code); err = quirc_decode(&code, &data); }
    if (!err) { decoded++; if ((size_t)data.payload_len == strlen(argv[2]) && !memcmp(data.payload,argv[2],(size_t)data.payload_len)) matched++; }
  }
  int identified = quirc_count(decoder);
  quirc_destroy(decoder);
  if (!strcmp(argv[2], "NO_CODE")) return decoded ? 1 : 0;
  if (matched != 1 || decoded != 1) fprintf(stderr, "identified=%d decoded=%d matched=%d\n", identified, decoded, matched);
  return matched == 1 && decoded == 1 ? 0 : decoded == 0 ? 1 : 3;
}
