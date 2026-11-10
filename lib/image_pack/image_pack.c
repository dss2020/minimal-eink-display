/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <eink/image_pack.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(image_pack, CONFIG_IMAGE_PACK_LOG_LEVEL);

/* Generated at build time by scripts/gen_image_pack.py (see CMakeLists.txt). */
static const uint8_t image_pack_data[] = {
#include <image_pack.bin.inc>
};

int image_pack_get_info(struct image_pack_info *info)
{
	int ret = image_pack_codec_parse(image_pack_data, sizeof(image_pack_data), info);

	if (ret != 0) {
		LOG_ERR("Invalid image pack (%d)", ret);
	}

	return ret;
}

size_t image_pack_frame_size(void)
{
	struct image_pack_info info;

	if (image_pack_codec_parse(image_pack_data, sizeof(image_pack_data), &info) != 0) {
		return 0;
	}

	return image_pack_codec_frame_size(&info);
}

int image_pack_decode(uint16_t index, uint8_t *buf, size_t buf_len)
{
	int ret = image_pack_codec_decode(image_pack_data, sizeof(image_pack_data), index, buf,
					  buf_len);

	if (ret != 0) {
		LOG_ERR("Decoding frame %u failed (%d)", index, ret);
	}

	return ret;
}
