/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "pixel_formats.xesli"
#define XE_TEXTURE_LOAD_64BPB_TO_128BPB XeXenosHalf8ToFloat32
#include "texture_load_64bpb_128bpb.xesli"
