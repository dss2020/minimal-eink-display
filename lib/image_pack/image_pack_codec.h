/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Portable image pack parser and decoder (no Zephyr dependencies).
 *
 * Pack format (all little-endian):
 *  - header, 12 bytes: magic "EIPK", version u8 (1), bpp u8 (1), width u16, height u16, count u16
 *  - @c count entries of 12 bytes: offset u32, length u32, method u8, 3 pad bytes
 *  - payloads
 *
 * A decoded frame is row-major, each row padded to a whole byte, MSB first, bit 1 = white.
 */

#ifndef IMAGE_PACK_CODEC_H_
#define IMAGE_PACK_CODEC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Image pack description. */
struct image_pack_info {
	/** Frame width in pixels. */
	uint16_t width;
	/** Frame height in pixels. */
	uint16_t height;
	/** Number of frames in the pack. */
	uint16_t count;
	/** Bits per pixel (currently always 1). */
	uint8_t bpp;
};

/**
 * @brief Validate a pack header and entry table and report its description.
 *
 * @param pack     Pack start.
 * @param pack_len Pack size in bytes.
 * @param info     Filled on success.
 *
 * @retval 0 on success.
 * @retval -EINVAL Bad arguments, bad magic, or truncated header/table.
 * @retval -ENOTSUP Unknown version or bpp.
 */
int image_pack_codec_parse(const uint8_t *pack, size_t pack_len, struct image_pack_info *info);

/**
 * @brief Size in bytes of one decoded frame ((width + 7) / 8 * height).
 */
size_t image_pack_codec_frame_size(const struct image_pack_info *info);

/**
 * @brief Decode one frame into @p buf.
 *
 * @retval 0 on success.
 * @retval -EINVAL Bad arguments, bad magic, or truncated header/table.
 * @retval -ENOTSUP Unknown version, bpp or method.
 * @retval -ENOENT @p index >= frame count.
 * @retval -ENOSPC @p buf_len smaller than the frame size.
 * @retval -EILSEQ Corrupt payload.
 */
int image_pack_codec_decode(const uint8_t *pack, size_t pack_len, uint16_t index, uint8_t *buf,
			    size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_PACK_CODEC_H_ */
