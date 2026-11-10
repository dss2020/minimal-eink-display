/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Access to the e-ink image pack embedded in the firmware.
 *
 * The pack is generated at build time from the pictures in
 * @kconfig{CONFIG_IMAGE_PACK_SOURCE_DIR} and linked into flash as read-only data.
 * Frames are decoded one at a time into a caller supplied buffer.
 */

#ifndef EINK_IMAGE_PACK_H_
#define EINK_IMAGE_PACK_H_

#include <stddef.h>
#include <stdint.h>

/* Defines struct image_pack_info; lib/image_pack is on the include path of the Zephyr build. */
#include <image_pack_codec.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the description of the embedded image pack.
 *
 * @param[out] info Filled on success.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p info is NULL or the pack is malformed.
 * @retval -ENOTSUP if the pack version or bits per pixel is not supported.
 */
int image_pack_get_info(struct image_pack_info *info);

/**
 * @brief Get the size in bytes of one decoded frame.
 *
 * Rows are padded to whole bytes, MSB first, bit 1 = white.
 *
 * @return Frame size in bytes, or 0 if the pack is invalid.
 */
size_t image_pack_frame_size(void);

/**
 * @brief Decode one frame of the pack.
 *
 * @param index   Frame index, 0 .. count - 1.
 * @param buf     Destination buffer.
 * @param buf_len Size of @p buf, at least image_pack_frame_size().
 *
 * @retval 0 on success.
 * @retval -ENOENT if @p index is out of range.
 * @retval -ENOSPC if @p buf_len is too small.
 * @retval -EILSEQ if the stored frame is corrupt.
 * @retval -EINVAL, -ENOTSUP for bad arguments or an unsupported pack.
 */
int image_pack_decode(uint16_t index, uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* EINK_IMAGE_PACK_H_ */
