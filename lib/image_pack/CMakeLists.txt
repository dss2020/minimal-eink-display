# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(image_pack.c image_pack_codec.c)

# Lets <eink/image_pack.h> find image_pack_codec.h
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Picture size as seen by the viewer. A shield or board sets the defaults.
if(CONFIG_IMAGE_PACK_WIDTH LESS 1 OR CONFIG_IMAGE_PACK_HEIGHT LESS 1)
  message(FATAL_ERROR "IMAGE_PACK: set CONFIG_IMAGE_PACK_WIDTH and CONFIG_IMAGE_PACK_HEIGHT "
                      "to the display size in pixels.")
endif()
set(_display_width ${CONFIG_IMAGE_PACK_WIDTH})
set(_display_height ${CONFIG_IMAGE_PACK_HEIGHT})

# Repository root (the Zephyr module directory) and picture folder
get_filename_component(_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(_pictures_dir "${CONFIG_IMAGE_PACK_SOURCE_DIR}" ABSOLUTE
                       BASE_DIR "${_repo_root}")

set(_globs)
foreach(_ext jpg jpeg png JPG JPEG PNG)
  list(APPEND _globs "${_pictures_dir}/*.${_ext}")
endforeach()
file(GLOB _pictures CONFIGURE_DEPENDS ${_globs})
list(REMOVE_DUPLICATES _pictures)  # case-insensitive file systems match both spellings
list(SORT _pictures)

if(CONFIG_IMAGE_PACK_FIT_CONTAIN)
  set(_fit contain)
else()
  set(_fit cover)
endif()

if(CONFIG_IMAGE_PACK_DITHER_NONE)
  set(_dither none)
else()
  set(_dither fs)
endif()

set(_pack_bin ${CMAKE_CURRENT_BINARY_DIR}/image_pack.bin)
set(_gen_script ${_repo_root}/scripts/gen_image_pack.py)

add_custom_command(
  OUTPUT ${_pack_bin}
  COMMAND ${PYTHON_EXECUTABLE} ${_gen_script}
          --input-dir ${_pictures_dir}
          --output ${_pack_bin}
          --width ${_display_width}
          --height ${_display_height}
          --fit ${_fit}
          --dither ${_dither}
          --preview-dir ${CMAKE_CURRENT_BINARY_DIR}/preview
  DEPENDS ${_gen_script} ${_pictures}
  COMMENT "Generating image pack from ${_pictures_dir} (${_display_width}x${_display_height})"
  VERBATIM
)

# Produces ${ZEPHYR_BINARY_DIR}/include/generated/image_pack.bin.inc, which is on the include
# path and is #included by image_pack.c.
generate_inc_file_for_target(
  ${ZEPHYR_CURRENT_LIBRARY}
  ${_pack_bin}
  ${ZEPHYR_BINARY_DIR}/include/generated/image_pack.bin.inc
)
