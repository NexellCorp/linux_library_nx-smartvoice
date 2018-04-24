/*
 * Copyright (C) 2017  Nexell Co., Ltd.
 * Author: Sungwoo, Park <swpark@nexell.co.kr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __NX_SMARTVOICE_H__
#define __NX_SMARTVOICE_H__

#ifdef __cplusplus
extern "C" {
#endif

struct ecnr_callback {
	void (*init)(int, int, char *);
	int (*process)(short *, short *, short *, short *, int);
	int (*post_process)(int, short *, int);
	void (*deinit)(void);
};

struct nx_smartvoice_config {
	bool use_feedback; /* use alsa feedback driver interface */
	int pdm_devnum; /* alsa pdm device number */
	int pdm_devnum2; /* alsa pdm device number */
	int ref_devnum; /* alsa reference device number */
	int feedback_devnum; /* alsa feedback device number, valid when use_feedback is true */
	int pdm_chnum; /* 2 or 4 */
	int pdm_gain; /* pdm gain value */
	int ref_resample_out_chnum; /* input 48KHz, 2channel, output 16KHz is fixed, output channel num(1 or 2) */
	bool check_trigger; /* if true, check encr_process return value and print trigger done */
	int trigger_done_ret_value; /* if check_trigger true and ecnr_process return this value, print trigger done */
	bool pass_after_trigger; /* if true, write only triggered data */
	bool verbose;

	struct ecnr_callback cb;
};

void *nx_voice_create_handle(void);
void nx_voice_close_handle(void *);
int nx_voice_start(void *, struct nx_smartvoice_config *);
void nx_voice_stop(void *);
int nx_voice_get_data(void *, short *, int);

#ifdef __cplusplus
}
#endif

#endif
