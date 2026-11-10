/* Host test: decode every frame of an image pack into OUT_DIR/frame_NNN.bin. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_pack_codec.h"

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s PACK_FILE OUT_DIR\n", argv[0]);
		return 2;
	}

	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return 1;
	}
	long len = ftell(f);
	if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return 1;
	}
	uint8_t *pack = malloc(len ? (size_t)len : 1);
	if (!pack || fread(pack, 1, (size_t)len, f) != (size_t)len) {
		fprintf(stderr, "read failed\n");
		fclose(f);
		free(pack);
		return 1;
	}
	fclose(f);

	struct image_pack_info info;
	int rc = image_pack_codec_parse(pack, (size_t)len, &info);
	if (rc != 0) {
		fprintf(stderr, "parse failed: %d\n", rc);
		free(pack);
		return 1;
	}
	size_t fsize = image_pack_codec_frame_size(&info);
	uint8_t *buf = malloc(fsize ? fsize : 1);
	if (!buf) {
		free(pack);
		return 1;
	}

	for (uint16_t i = 0; i < info.count; i++) {
		rc = image_pack_codec_decode(pack, (size_t)len, i, buf, fsize);
		if (rc != 0) {
			fprintf(stderr, "decode frame %u failed: %d\n", (unsigned)i, rc);
			free(buf);
			free(pack);
			return 1;
		}
		char path[4096];
		snprintf(path, sizeof(path), "%s/frame_%03u.bin", argv[2], (unsigned)i);
		FILE *o = fopen(path, "wb");
		if (!o || fwrite(buf, 1, fsize, o) != fsize) {
			perror(path);
			if (o) {
				fclose(o);
			}
			free(buf);
			free(pack);
			return 1;
		}
		fclose(o);
	}
	free(buf);
	free(pack);
	return 0;
}
