/* SPDX-License-Identifier: GPL-2.0 */
/*
 * These are the H.264 state controls for use with stateless H.264
 * codec drivers.
 *
 * It turns out that these structs are not stable yet and will undergo
 * more changes. So keep them private until they are stable and ready to
 * become part of the official public API.
 */
#include <linux/v4l2-controls.h>

#ifndef _H264_CTRLS_H_
#define _H264_CTRLS_H_

#include <linux/videodev2.h>