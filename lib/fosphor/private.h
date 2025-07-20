/*
 * private.h
 *
 * Private fosphor definitions
 *
 * Copyright (C) 2013-2021 Sylvain Munaut
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

/*! \defgroup private
 *  @{
 */

/*! \file private.h
 *  \brief Private fosphor definitions
 */


#define FOSPHOR_FFT_LEN_LOG_DEFAULT	10
#define FOSPHOR_FFT_LEN_DEFAULT		(1<<FOSPHOR_FFT_LEN_LOG_DEFAULT)

#define FOSPHOR_FFT_MULT_BATCH	16
#define FOSPHOR_FFT_MAX_BATCH	1024

#define FOSPHOR_FFT_LEN_LOG	fosphor_fft_len_log_get()
#define FOSPHOR_FFT_LEN		fosphor_fft_len_get()

int fosphor_fft_len_log_get(void);
int fosphor_fft_len_get(void);
void fosphor_fft_len_set(int len);
int fosphor_fft_len_validate(int len);

struct fosphor_cl_state;
struct fosphor_gl_state;

struct fosphor
{
	struct fosphor_cl_state *cl;
	struct fosphor_gl_state *gl;

#define FLG_FOSPHOR_USE_CLGL_SHARING	(1<<0)
	int flags;

	float *fft_win;
	int fft_len;

	float *img_waterfall;
	float *img_histogram;
	float *buf_spectrum;

	struct {
		int db_ref;
		int db_per_div;
		float scale;
		float offset;
	} power;

	struct {
		double center;
		double span;
	} frequency;
};


/*! @} */
