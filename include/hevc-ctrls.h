/* SPDX-License-Identifier: GPL-2.0 */
/*
 * These are the HEVC state controls for use with stateless HEVC
 * codec drivers.
 *
 * It turns out that these structs are not stable yet and will undergo
 * more changes. So keep them private until they are stable and ready to
 * become part of the official public API.
 */
#include <linux/v4l2-controls.h>

#ifndef _HEVC_CTRLS_H_
#define _HEVC_CTRLS_H_