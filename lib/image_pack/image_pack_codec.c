/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "image_pack_codec.h"

#define HDR_SIZE   12U
#define ENTRY_SIZE 12U

#define METHOD_RAW  0U
#define METHOD_LZSS 1U

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

int image_pack_codec_parse(const uint8_t *pack, size_t pack_len, struct image_pack_info *info)
{
	size_t table_len;

	if (pack == NULL || info == NULL || pack_len < HDR_SIZE) {
		return -EINVAL;
	}
	if (memcmp(pack, "EIPK", 4) != 0) {
		return -EINVAL;
	}
	if (pack[4] != 1U || pack[5] != 1U) {
		return -ENOTSUP;
	}

	info->bpp = pack[5];
	info->width = rd16(&pack[6]);
	info->height = rd16(&pack[8]);
	info->count = rd16(&pack[10]);

	if (info->width == 0U || info->height == 0U) {
		return -EINVAL;
	}

	table_len = (size_t)info->count * ENTRY_SIZE;
	if (table_len > pack_len - HDR_SIZE) {
		return -EINVAL;
	}

	return 0;
}

size_t image_pack_codec_frame_size(const struct image_pack_info *info)
{
	if (info == NULL) {
		return 0;
	}

	return (((size_t)info->width + 7U) / 8U) * (size_t)info->height;
}

static int decode_lzss(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
	size_t ip = 0;
	size_t op = 0;

	while (op < out_len) {
		uint8_t flags;
		unsigned int bit;

		if (ip >= in_len) {
			return -EILSEQ;
		}
		flags = in[ip++];

		for (bit = 0; bit < 8U && op < out_len; bit++) {
			if ((flags >> bit) & 1U) {
				if (ip >= in_len) {
					return -EILSEQ;
				}
				out[op++] = in[ip++];
			} else {
				size_t offset;
				size_t len;
				size_t i;
				uint16_t v;

				if (in_len - ip < 2U) {
					return -EILSEQ;
				}
				v = rd16(&in[ip]);
				ip += 2U;
				offset = ((size_t)v >> 4) + 1U;
				len = ((size_t)v & 0xFU) + 3U;

				if (offset > op || len > out_len - op) {
					return -EILSEQ;
				}
				for (i = 0; i < len; i++) {
					out[op] = out[op - offset];
					op++;
				}
			}
		}
	}

	return 0;
}

int image_pack_codec_decode(const uint8_t *pack, size_t pack_len, uint16_t index, uint8_t *buf,
			    size_t buf_len)
{
	struct image_pack_info info;
	const uint8_t *entry;
	size_t frame_size;
	size_t offset;
	size_t length;
	int ret;

	if (buf == NULL) {
		return -EINVAL;
	}

	ret = image_pack_codec_parse(pack, pack_len, &info);
	if (ret != 0) {
		return ret;
	}
	if (index >= info.count) {
		return -ENOENT;
	}

	frame_size = image_pack_codec_frame_size(&info);
	if (buf_len < frame_size) {
		return -ENOSPC;
	}

	entry = &pack[HDR_SIZE + (size_t)index * ENTRY_SIZE];
	offset = rd32(&entry[0]);
	length = rd32(&entry[4]);

	if (entry[8] != METHOD_RAW && entry[8] != METHOD_LZSS) {
		return -ENOTSUP;
	}
	if (offset > pack_len || length > pack_len - offset) {
		return -EILSEQ;
	}

	if (entry[8] == METHOD_RAW) {
		if (length != frame_size) {
			return -EILSEQ;
		}
		memcpy(buf, &pack[offset], frame_size);
		return 0;
	}

	return decode_lzss(&pack[offset], length, buf, frame_size);
}
