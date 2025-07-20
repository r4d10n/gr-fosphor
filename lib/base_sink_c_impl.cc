/* -*- c++ -*- */
/*
 * Copyright 2013-2021 Sylvain Munaut <tnt@246tNt.com>
 *
 * This file is part of gr-fosphor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>

#include <gnuradio/io_signature.h>
#include <gnuradio/thread/thread.h>

#include "fifo.h"
#include "base_sink_c_impl.h"

#ifdef ENABLE_GLEW
# include <GL/glew.h>
#endif

extern "C" {
#include "fosphor/fosphor.h"
#include "fosphor/gl_platform.h"
}


namespace gr {
  namespace fosphor {

base_sink_c::base_sink_c(const char *name)
  : gr::sync_block(name,
                   gr::io_signature::make(1, 1, sizeof(gr_complex)),
                   gr::io_signature::make(0, 0, 0))
{
	/* Register message ports */
	message_port_register_out(pmt::mp("freq"));
}


gr::thread::mutex base_sink_c_impl::s_boot_mutex;

const int base_sink_c_impl::k_db_per_div[] = {1, 2, 5, 10, 20};


base_sink_c_impl::base_sink_c_impl()
  : d_db_ref(0), d_db_per_div_idx(3),
    d_zoom_enabled(false), d_zoom_center(0.5), d_zoom_width(0.2),
    d_ratio(0.35f), d_frozen(false), d_active(false), d_visible(false),
    d_frequency(), d_fft_window(gr::fft::window::WIN_BLACKMAN_hARRIS), d_fft_size(1024)
{
	/* Init FIFO */
	this->d_fifo = new fifo(2 * 1024 * 1024);

	/* Init render options */
	this->d_render_main = new fosphor_render();
	fosphor_render_defaults(this->d_render_main);

	this->d_render_zoom = new fosphor_render();
	fosphor_render_defaults(this->d_render_zoom);
	this->d_render_zoom->options &= ~(FRO_LABEL_PWR | FRO_LABEL_TIME);
}

base_sink_c_impl::~base_sink_c_impl()
{
	delete this->d_render_zoom;
	delete this->d_render_main;
	delete this->d_fifo;
}


void base_sink_c_impl::worker()
{
	/* Zero-init */
	this->d_fosphor = NULL;

	/* Init GL context */
	this->glctx_init();

#ifdef ENABLE_GLEW
	GLenum glew_err = glewInit();
	if (glew_err != GLEW_OK) {
		GR_LOG_ERROR(d_logger, boost::format("GLEW initialization error : %s") % glewGetErrorString(glew_err));
		goto error;
	}
#endif

	/* Init fosphor */
	{
		/* (prevent // init of multiple instance to be gentle on the OpenCL
		 *  implementations that don't like this) */
		gr::thread::scoped_lock guard(s_boot_mutex);

		this->d_fosphor = fosphor_init();
		if (!this->d_fosphor) {
			GR_LOG_ERROR(d_logger, "Failed to initialize fosphor");
			goto error;
		}
	}

	this->settings_apply(~SETTING_DIMENSIONS);

	/* Main loop */
	while (this->d_active)
	{
		this->render();
		this->glctx_poll();
	}

error:
	/* Cleanup fosphor */
	if (this->d_fosphor)
		fosphor_release(this->d_fosphor);

	/* And GL context */
	this->glctx_fini();
}

void base_sink_c_impl::_worker(base_sink_c_impl *obj)
{
        obj->worker();
}


void
base_sink_c_impl::render(void)
{
	const int fft_len    = this->d_fft_size;
	const int batch_mult = 16;
	const int batch_max  = 1024;
	const int max_iter   = 8;

	int i, tot_len;

	/* Handle pending settings */
	this->settings_apply(this->settings_get_and_reset_changed());

	/* Process as much we can */
	tot_len = this->d_fifo->used();

	for (i=0; i<max_iter && tot_len; i++)
	{
		gr_complex *data;
		int len;

		/* How much can we get from FIFO in one block */
		len = tot_len;
		if (len > this->d_fifo->read_max_size())
			len = this->d_fifo->read_max_size();

		/* Adapt to valid size for fosphor */
		len &= ~((batch_mult * fft_len) - 1);
		if (len > (batch_max * fft_len))
			len = batch_max * fft_len;

		/* Is it worth it ? */
		tot_len -= len;

		if (!len)
			break;

		/* Send to process (if not frozen) */
		if (!this->d_frozen) {
			data = this->d_fifo->read_peek(len, false);
			fosphor_process(this->d_fosphor, data, len);
		}

		/* Discard */
		this->d_fifo->read_discard(len);
	}

	/* Are we visible ? */
	{
		gr::thread::scoped_lock guard(this->d_render_mutex);

		if (this->d_visible) {
			/* Clear everything */
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear(GL_COLOR_BUFFER_BIT);

			/* Draw */
			fosphor_draw(this->d_fosphor, this->d_render_main);

			if (this->d_zoom_enabled)
				fosphor_draw(this->d_fosphor, this->d_render_zoom);

			/* Done, swap buffer */
			this->glctx_swap();
		}
	}

	if (!this->d_visible) {
		/* If hidden, we can't draw or swap buffer, so just wait a bit */
		boost::this_thread::sleep_for(boost::chrono::milliseconds(10));
	}
}


void
base_sink_c_impl::settings_mark_changed(uint32_t setting)
{
	gr::thread::scoped_lock lock(this->d_settings_mutex);
	this->d_settings_changed |= setting;
}

uint32_t
base_sink_c_impl::settings_get_and_reset_changed(void)
{
	gr::thread::scoped_lock lock(this->d_settings_mutex);
	uint32_t v = this->d_settings_changed;
	this->d_settings_changed = 0;
	return v;
}

void
base_sink_c_impl::settings_apply(uint32_t settings)
{
	if (settings & SETTING_DIMENSIONS)
	{
		this->glctx_update();

		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0.0, (double)this->d_width, 0.0, (double)this->d_height, -1.0, 1.0);

		glViewport(0, 0, this->d_width, this->d_height);
	}

	if (settings & SETTING_POWER_RANGE) {
		fosphor_set_power_range(this->d_fosphor,
			this->d_db_ref,
			this->k_db_per_div[this->d_db_per_div_idx]
		);
	}

	if (settings & SETTING_FREQUENCY_RANGE) {
		fosphor_set_frequency_range(this->d_fosphor,
			this->d_frequency.center,
			this->d_frequency.span
		);
	}

	if (settings & SETTING_FFT_WINDOW) {
		std::vector<float> window =
			gr::fft::window::build(this->d_fft_window, this->d_fft_size, 6.76);
		fosphor_set_fft_window(this->d_fosphor, window.data());
	}

	if (settings & SETTING_FFT_SIZE) {
		fosphor_fft_len_set(this->d_fft_size);
		/* Rebuild window with new size */
		std::vector<float> window =
			gr::fft::window::build(this->d_fft_window, this->d_fft_size, 6.76);
		fosphor_set_fft_window(this->d_fosphor, window.data());
	}

	if (settings & (SETTING_DIMENSIONS | SETTING_RENDER_OPTIONS))
	{
		float f;

		if (this->d_zoom_enabled) {
			int a = (int)(this->d_width * 0.65f);
			this->d_render_main->width = a;
			this->d_render_main->options |= FRO_CHANNELS;
			this->d_render_main->options &= ~FRO_COLOR_SCALE;
			this->d_render_zoom->pos_x = a - 10;
			this->d_render_zoom->width = this->d_width - a + 10;
		} else {
			this->d_render_main->width = this->d_width;
			this->d_render_main->options &= ~FRO_CHANNELS;
			this->d_render_main->options |= FRO_COLOR_SCALE;
		}

		this->d_render_main->height = this->d_height;
		this->d_render_zoom->height = this->d_height;

		this->d_render_main->histo_wf_ratio = this->d_ratio;
		this->d_render_zoom->histo_wf_ratio = this->d_ratio;

		this->d_render_main->channels[0].enabled = this->d_zoom_enabled;
		this->d_render_main->channels[0].center  = (float)this->d_zoom_center;
		this->d_render_main->channels[0].width   = (float)this->d_zoom_width;

		this->d_render_zoom->freq_center = (float)this->d_zoom_center;
		this->d_render_zoom->freq_span   = (float)this->d_zoom_width;

		fosphor_render_refresh(this->d_render_main);
		fosphor_render_refresh(this->d_render_zoom);
	}
}


void
base_sink_c_impl::cb_reshape(int width, int height)
{
	this->d_width    = width;
	this->d_height   = height;
	this->settings_mark_changed(SETTING_DIMENSIONS);
}

void
base_sink_c_impl::cb_visibility(bool visible)
{
	gr::thread::scoped_lock guard(this->d_render_mutex);
	this->d_visible = visible;
}


void
base_sink_c_impl::execute_ui_action(enum ui_action_t action)
{
	switch (action) {
	case DB_PER_DIV_UP:
		if (this->d_db_per_div_idx < 4)
			this->d_db_per_div_idx++;
		break;

	case DB_PER_DIV_DOWN:
		if (this->d_db_per_div_idx > 0)
			this->d_db_per_div_idx--;
		break;

	case REF_UP:
		this->d_db_ref += k_db_per_div[this->d_db_per_div_idx];
		break;

	case REF_DOWN:
		this->d_db_ref -= k_db_per_div[this->d_db_per_div_idx];
		break;

	case ZOOM_TOGGLE:
		this->d_zoom_enabled ^= 1;
		break;

	case ZOOM_WIDTH_UP:
		if (this->d_zoom_enabled)
			this->d_zoom_width *= 2.0;
		break;

	case ZOOM_WIDTH_DOWN:
		if (this->d_zoom_enabled)
			this->d_zoom_width /= 2.0;
		break;

	case ZOOM_CENTER_UP:
		if (this->d_zoom_enabled)
			this->d_zoom_center += this->d_zoom_width / 8.0;
		break;

	case ZOOM_CENTER_DOWN:
		if (this->d_zoom_enabled)
			this->d_zoom_center -= this->d_zoom_width / 8.0;
		break;

	case RATIO_UP:
		if (this->d_ratio < 0.8f)
			this->d_ratio += 0.05f;
		break;

	case RATIO_DOWN:
		if (this->d_ratio > 0.2f)
			this->d_ratio -= 0.05f;
		break;

	case FREEZE_TOGGLE:
		this->d_frozen ^= 1;
		break;
	}

	this->settings_mark_changed(
		SETTING_POWER_RANGE |
		SETTING_RENDER_OPTIONS
	);
}

void
base_sink_c_impl::execute_mouse_action(enum mouse_action_t action, int x, int y)
{
	if (action != CLICK)
		return;

	/* Identify position */
	int in_main = fosphor_render_pos_inside(this->d_render_main, x, y);
	int in_zoom = this->d_zoom_enabled ? fosphor_render_pos_inside(this->d_render_zoom, x, y) : 0;

	/* Send frequency */
	if (in_main & 1)
	{
		double freq = fosphor_pos2freq(this->d_fosphor, this->d_render_main, x);
		message_port_pub(pmt::mp("freq"), pmt::cons(pmt::mp("freq"), pmt::from_double(freq)));
	}
	else if (in_zoom & 1)
	{
		double freq = fosphor_pos2freq(this->d_fosphor, this->d_render_zoom, x);
		message_port_pub(pmt::mp("freq"), pmt::cons(pmt::mp("freq"), pmt::from_double(freq)));
	}
}

void
base_sink_c_impl::set_frequency_range(const double center, const double span)
{
	this->d_frequency.center = center;
	this->d_frequency.span   = span;
	this->settings_mark_changed(SETTING_FREQUENCY_RANGE);
}

void
base_sink_c_impl::set_frequency_center(const double center)
{
	this->d_frequency.center = center;
	this->settings_mark_changed(SETTING_FREQUENCY_RANGE);
}

void
base_sink_c_impl::set_frequency_span(const double span)
{
	this->d_frequency.span   = span;
	this->settings_mark_changed(SETTING_FREQUENCY_RANGE);
}

void
base_sink_c_impl::set_fft_window(const gr::fft::window::win_type win)
{
	if (win == this->d_fft_window)	/* Reloading FFT window takes time */
		return;			/* avoid doing it if possible */

	this->d_fft_window = win;
	this->settings_mark_changed(SETTING_FFT_WINDOW);
}

void
base_sink_c_impl::set_fft_size(const int size)
{
	if (size == this->d_fft_size)
		return;

	if (size == 512 || size == 1024 || size == 2048 || 
	    size == 4096 || size == 8192 || size == 16384 || size == 32768) {
		this->d_fft_size = size;
		this->settings_mark_changed(SETTING_FFT_SIZE);
	}
}


int
base_sink_c_impl::work(
	int noutput_items,
	gr_vector_const_void_star &input_items,
	gr_vector_void_star &output_items)
{
	const gr_complex *in = (const gr_complex *) input_items[0];
	gr_complex *dst;
	int l, mw;

	/* How much can we hope to write */
	l = noutput_items;
	mw = this->d_fifo->write_max_size();

	if (l > mw)
		l = mw;
	if (!l)
		return 0;

	/* Get a pointer */
	dst = this->d_fifo->write_prepare(l, true);
	if (!dst)
		return 0;

	/* Do the copy */
	memcpy(dst, in, sizeof(gr_complex) * l);
	this->d_fifo->write_commit(l);

	/* Report what we took */
	return l;
}

bool base_sink_c_impl::start()
{
	bool rv = base_sink_c::start();
	if (!this->d_active) {
		this->d_active = true;
		this->d_worker = gr::thread::thread(_worker, this);
	}
	return rv;
}

bool base_sink_c_impl::stop()
{
	bool rv = base_sink_c::stop();
	if (this->d_active) {
		this->d_active = false;
		this->d_worker.join();
	}
	return rv;
}

  } /* namespace fosphor */
} /* namespace gr */
