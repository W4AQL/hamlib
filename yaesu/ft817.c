/*
 * hamlib - (C) Frank Singleton 2000,2001 (vk3fcs@ix.netcom.com)
 *
 * ft817.c - (C) Chris Karpinsky 2001 (aa1vl@arrl.net)
 * This shared library provides an API for communicating
 * via serial interface to an FT-817 using the "CAT" interface.
 * The starting point for this code was Frank's ft847 implementation.
 *
 * Then, Tommi OH2BNS improved the code a lot in the framework of the
 * FT-857 backend. These improvements have now (August 2005) been
 * copied back and adopted for the FT-817.
 *
 *
 *    $Id: ft817.c,v 1.12 2005-09-04 10:44:22 csete Exp $  
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 */

/*
 * Unimplemented features supported by the FT-817:
 *
 *   - RIT ON/OFF without touching the RIT offset. This would
 *     need frontend support (eg. a new RIG_FUNC_xxx)
 *
 *   - RX status command returns info that is not used:
 *      - discriminator centered (yes/no flag)
 *      - received ctcss/dcs matched (yes/no flag)                     TBC
 *
 *   - TX status command returns info that is not used:
 *      - split on/off flag; actually this could have been used
 *        for get_split_vfo, but the flag is valid only when
 *        PTT is ON.
 *      - high swr flag
 *
 * Todo / tocheck list (oz9aec):
 * - check power meter reading, add power2mW, mW2power
 * - test get_dcd; rigctl does not support it?
 * - squelch
 * - the many "fixme" stuff around
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>  	/* String function definitions */
#include <unistd.h>  	/* UNIX standard function definitions */

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "hamlib/rig.h"
#include "serial.h"
#include "yaesu.h"
#include "ft817.h"
#include "misc.h"
#include "tones.h"
#include "bandplan.h"


/* Native ft817 cmd set prototypes. These are READ ONLY as each */
/* rig instance will copy from these and modify if required . */
/* Complete sequences (1) can be read and used directly as a cmd sequence . */
/* Incomplete sequences (0) must be completed with extra parameters */
/* eg: mem number, or freq etc.. */
static const yaesu_cmd_set_t ncmd[] = {
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x00 } }, /* lock on */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x80 } }, /* lock off */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x08 } }, /* ptt on */
	{ 1, { 0x00, 0x00, 0x00, 0x01, 0x88 } }, /* ptt off */
	{ 0, { 0x00, 0x00, 0x00, 0x00, 0x01 } }, /* set freq */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x07 } }, /* mode set main LSB */
	{ 1, { 0x01, 0x00, 0x00, 0x00, 0x07 } }, /* mode set main USB */
	{ 1, { 0x02, 0x00, 0x00, 0x00, 0x07 } }, /* mode set main CW */
	{ 1, { 0x03, 0x00, 0x00, 0x00, 0x07 } }, /* mode set main CWR */
	{ 1, { 0x04, 0x00, 0x00, 0x00, 0x07 } }, /* mode set main AM */
	{ 1, { 0x08, 0x00, 0x00, 0x00, 0x07 } }, /* mode set main FM */
	{ 1, { 0x88, 0x00, 0x00, 0x00, 0x07 } }, /* mode set main FM-N */
	{ 1, { 0x0a, 0x00, 0x00, 0x00, 0x07 } }, /* mode set main DIG */
	{ 1, { 0x0c, 0x00, 0x00, 0x00, 0x07 } }, /* mode set main PKT */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x05 } }, /* clar on */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x85 } }, /* clar off */
	{ 0, { 0x00, 0x00, 0x00, 0x00, 0xf5 } }, /* set clar freq */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x81 } }, /* toggle vfo a/b */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x02 } }, /* split on */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x82 } }, /* split off */
	{ 1, { 0x09, 0x00, 0x00, 0x00, 0x09 } }, /* set RPT shift MINUS */
	{ 1, { 0x49, 0x00, 0x00, 0x00, 0x09 } }, /* set RPT shift PLUS */
	{ 1, { 0x89, 0x00, 0x00, 0x00, 0x09 } }, /* set RPT shift SIMPLEX */
	{ 0, { 0x00, 0x00, 0x00, 0x00, 0xf9 } }, /* set RPT offset freq */
	{ 1, { 0x0a, 0x00, 0x00, 0x00, 0x0a } }, /* set DCS on */
	{ 1, { 0x2a, 0x00, 0x00, 0x00, 0x0a } }, /* set CTCSS on */
	{ 1, { 0x4a, 0x00, 0x00, 0x00, 0x0a } }, /* set CTCSS encoder on */
	{ 1, { 0x8a, 0x00, 0x00, 0x00, 0x0a } }, /* set CTCSS/DCS off */
	{ 0, { 0x00, 0x00, 0x00, 0x00, 0x0b } }, /* set CTCSS tone */
	{ 0, { 0x00, 0x00, 0x00, 0x00, 0x0c } }, /* set DCS code */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0xe7 } }, /* get RX status  */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0xf7 } }, /* get TX status  */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x03 } }, /* get FREQ and MODE status */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x00 } }, /* pwr wakeup sequence */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x0f } }, /* pwr on */
	{ 1, { 0x00, 0x00, 0x00, 0x00, 0x8f } }, /* pwr off */
};

static const tone_t static_ft817_dcs_list[] = {
	23,  25,  26,  31,  32,  36,  43,  47, 51,  53,
	54,  65,  71,  72,  73,  74, 114, 115, 116, 122,
	125, 131, 132, 134, 143, 145, 152, 155, 156, 162,
	165, 172, 174, 205, 212, 223, 225, 226, 243, 244,
	245, 246, 251, 252, 255, 261, 263, 265, 266, 271,
	274, 306, 311, 315, 325, 331, 332, 343,	346, 351,
	356, 364, 365, 371, 411, 412, 413, 423, 431, 432,
	445, 446, 452, 454, 455, 462, 464, 465, 466, 503,
	506, 516, 523, 526, 532, 546, 565, 606, 612, 624,
	627, 631, 632, 654, 662, 664, 703, 712, 723, 731,
	732, 734, 743, 754, 0	
};

#define FT817_ALL_RX_MODES      (RIG_MODE_AM|RIG_MODE_CW|RIG_MODE_CWR|RIG_MODE_PKTFM|\
                                 RIG_MODE_USB|RIG_MODE_LSB|RIG_MODE_RTTY|RIG_MODE_FM)
#define FT817_SSB_CW_RX_MODES   (RIG_MODE_CW|RIG_MODE_CWR|RIG_MODE_USB|RIG_MODE_LSB|RIG_MODE_RTTY)
#define FT817_CWN_RX_MODES      (RIG_MODE_CW|RIG_MODE_CWR|RIG_MODE_RTTY)
#define FT817_AM_FM_RX_MODES    (RIG_MODE_AM|RIG_MODE_FM|RIG_MODE_PKTFM)

#define FT817_OTHER_TX_MODES    (RIG_MODE_CW|RIG_MODE_CWR|RIG_MODE_USB|\
                                 RIG_MODE_LSB|RIG_MODE_RTTY|RIG_MODE_FM)
#define FT817_AM_TX_MODES       (RIG_MODE_AM)

#define FT817_VFO_ALL           (RIG_VFO_A|RIG_VFO_B)
#define FT817_ANTS              0

const struct rig_caps ft817_caps = {
	.rig_model = 		RIG_MODEL_FT817,
	.model_name = 	        "FT-817", 
	.mfg_name = 		"Yaesu", 
	.version = 		"0.2", 
	.copyright = 		"LGPL",
	.status = 		RIG_STATUS_BETA,
	.rig_type = 		RIG_TYPE_TRANSCEIVER,
	.ptt_type = 		RIG_PTT_RIG,
	.dcd_type = 		RIG_DCD_RIG,
	.port_type = 		RIG_PORT_SERIAL,
	.serial_rate_min = 	4800,
	.serial_rate_max = 	38400,
	.serial_data_bits = 	8,
	.serial_stop_bits = 	2,
	.serial_parity = 	RIG_PARITY_NONE,
	.serial_handshake = 	RIG_HANDSHAKE_NONE, 
	.write_delay = 	        FT817_WRITE_DELAY,
	.post_write_delay = 	FT817_POST_WRITE_DELAY,
	.timeout = 		FT817_TIMEOUT,
	.retry = 		0, 
	.has_get_func =         RIG_FUNC_NONE,
	.has_set_func = 	RIG_FUNC_LOCK | RIG_FUNC_TONE | RIG_FUNC_TSQL,
	.has_get_level = 	RIG_LEVEL_STRENGTH | RIG_LEVEL_RAWSTR | RIG_LEVEL_RFPOWER,
	.has_set_level = 	RIG_LEVEL_NONE,
	.has_get_parm = 	RIG_PARM_NONE,
	.has_set_parm = 	RIG_PARM_NONE,
	.level_gran = 	        {},                     /* granularity */
	.parm_gran = 		{},
	.ctcss_list =    	static_common_ctcss_list,
	.dcs_list = 		static_ft817_dcs_list,   /* only 104 out of 106 supported */
	.preamp = 		{ RIG_DBLST_END, },
	.attenuator =    	{ RIG_DBLST_END, },
	.max_rit = 		Hz(9990),
	.max_xit = 		Hz(0),
	.max_ifshift = 	        Hz(0),
	.vfo_ops =              RIG_OP_TOGGLE,
	.targetable_vfo = 	0,
	.transceive = 	        RIG_TRN_OFF,
	.bank_qty = 		0,
	.chan_desc_sz = 	0,
	.chan_list =            { RIG_CHAN_END, },

	.rx_range_list1 =  { 
		{kHz(100),MHz(56), FT817_ALL_RX_MODES,-1,-1},
		{MHz(76), MHz(108),RIG_MODE_WFM,      -1,-1},
		{MHz(118),MHz(164),FT817_ALL_RX_MODES,-1,-1},
		{MHz(420),MHz(470),FT817_ALL_RX_MODES,-1,-1},
		RIG_FRNG_END, 
	},
	.tx_range_list1 =  {
		FRQ_RNG_HF(1, FT817_OTHER_TX_MODES, W(0.5), W(5), FT817_VFO_ALL, FT817_ANTS),
		FRQ_RNG_HF(1, FT817_AM_TX_MODES, W(0.5),W(1.5), FT817_VFO_ALL, FT817_ANTS),

		FRQ_RNG_6m(1, FT817_OTHER_TX_MODES, W(0.5), W(5), FT817_VFO_ALL, FT817_ANTS),
		FRQ_RNG_6m(1, FT817_AM_TX_MODES, W(0.5),W(1.5), FT817_VFO_ALL, FT817_ANTS),

		FRQ_RNG_2m(1, FT817_OTHER_TX_MODES, W(0.5),W(5),FT817_VFO_ALL,FT817_ANTS),
		FRQ_RNG_2m(1, FT817_AM_TX_MODES, W(0.5),W(1.5),FT817_VFO_ALL,FT817_ANTS),

		FRQ_RNG_70cm(1, FT817_OTHER_TX_MODES, W(0.5),W(5),FT817_VFO_ALL,FT817_ANTS),
		FRQ_RNG_70cm(1, FT817_AM_TX_MODES, W(0.5),W(1.5),FT817_VFO_ALL,FT817_ANTS),

		RIG_FRNG_END, 
	},


	.rx_range_list2 =  { 
		{kHz(100),MHz(56), FT817_ALL_RX_MODES,-1,-1},
		{MHz(76), MHz(108),RIG_MODE_WFM,      -1,-1},
		{MHz(118),MHz(164),FT817_ALL_RX_MODES,-1,-1},
		{MHz(420),MHz(470),FT817_ALL_RX_MODES,-1,-1},
		RIG_FRNG_END, 
	},

	.tx_range_list2 =  {
		FRQ_RNG_HF(2, FT817_OTHER_TX_MODES, W(0.5), W(5), FT817_VFO_ALL, FT817_ANTS),
		FRQ_RNG_HF(2, FT817_AM_TX_MODES, W(0.5),W(1.5), FT817_VFO_ALL, FT817_ANTS),
	/* FIXME: 60 meters in US version */

		FRQ_RNG_6m(2, FT817_OTHER_TX_MODES, W(0.5), W(5), FT817_VFO_ALL, FT817_ANTS),
		FRQ_RNG_6m(2, FT817_AM_TX_MODES, W(0.5),W(1.5), FT817_VFO_ALL, FT817_ANTS),

		FRQ_RNG_2m(2, FT817_OTHER_TX_MODES, W(0.5),W(5),FT817_VFO_ALL,FT817_ANTS),
		FRQ_RNG_2m(2, FT817_AM_TX_MODES, W(0.5),W(1.5),FT817_VFO_ALL,FT817_ANTS),

		FRQ_RNG_70cm(2, FT817_OTHER_TX_MODES, W(0.5),W(5),FT817_VFO_ALL,FT817_ANTS),
		FRQ_RNG_70cm(2, FT817_AM_TX_MODES, W(0.5),W(1.5),FT817_VFO_ALL,FT817_ANTS),

		RIG_FRNG_END, 
	},

	.tuning_steps =  {
		{FT817_SSB_CW_RX_MODES,Hz(10)},
		{FT817_AM_FM_RX_MODES|RIG_MODE_WFM,Hz(100)},
		RIG_TS_END,
	},  

	.filters = {
		{FT817_SSB_CW_RX_MODES, kHz(2.2)},  /* normal passband */
		{FT817_CWN_RX_MODES, 500},          /* CW and RTTY narrow */
		{RIG_MODE_AM, kHz(6)},              /* AM normal */
		{RIG_MODE_FM|RIG_MODE_PKTFM, kHz(9)},
		{RIG_MODE_WFM, kHz(15)},
		RIG_FLT_END,
	},

	.priv = 		NULL,
	.rig_init = 		ft817_init,
	.rig_cleanup =          ft817_cleanup, 
	.rig_open = 		ft817_open, 
	.rig_close = 		ft817_close, 
	.set_freq = 		ft817_set_freq,
	.get_freq = 		ft817_get_freq,
	.set_mode = 		ft817_set_mode,
	.get_mode = 		ft817_get_mode,
	.set_vfo = 		NULL,
	.get_vfo = 		NULL,
	.set_ptt = 		ft817_set_ptt,
	.get_ptt = 		ft817_get_ptt,
	.get_dcd = 		ft817_get_dcd,
	.set_rptr_shift = 	ft817_set_rptr_shift,
	.get_rptr_shift = 	NULL,
	.set_rptr_offs = 	ft817_set_rptr_offs,
	.get_rptr_offs = 	NULL,
	.set_split_freq = 	NULL,
	.get_split_freq = 	NULL,
	.set_split_mode = 	NULL,
	.get_split_mode = 	NULL,
	.set_split_vfo = 	ft817_set_split_vfo,
	.get_split_vfo =	NULL, /* possible, but works only if PTT is ON */
	.set_rit = 		ft817_set_rit,
	.get_rit = 		NULL,
	.set_xit = 		NULL,
	.get_xit = 		NULL,
	.set_ts = 		NULL,
	.get_ts = 		NULL,
	.set_dcs_code = 	ft817_set_dcs_code,
	.get_dcs_code = 	NULL,
	.set_tone =             NULL,
	.get_tone =             NULL,
	.set_ctcss_tone = 	ft817_set_ctcss_tone,
	.get_ctcss_tone = 	NULL,
	.set_tone =             NULL,
	.get_tone =             NULL,
	.set_dcs_sql = 	        ft817_set_dcs_sql,
	.get_dcs_sql =          NULL,
	.set_tone_sql =         NULL,
	.get_tone_sql =         NULL,
	.set_ctcss_sql = 	ft817_set_ctcss_sql,
	.get_ctcss_sql = 	NULL,
	.power2mW =             NULL,
	.mW2power =             NULL,
	.set_powerstat = 	ft817_set_powerstat,
	.get_powerstat = 	NULL,
	.reset = 		NULL,
	.set_ant = 		NULL,
	.get_ant = 		NULL,
	.set_level = 		NULL,
	.get_level = 		ft817_get_level,
	.set_func = 		ft817_set_func,
	.get_func = 		NULL,
	.set_parm = 		NULL,
	.get_parm = 		NULL,
	.set_ext_level =        NULL,
	.get_ext_level =        NULL,
	.set_ext_parm =         NULL,
	.get_ext_parm =         NULL,
	.set_conf =             NULL,
	.get_conf =             NULL,
	.send_dtmf =            NULL,
	.recv_dtmf =            NULL,
	.send_morse =           NULL,
	.set_bank =             NULL,
	.set_mem =              NULL,
	.get_mem =              NULL,
	.vfo_op =               ft817_vfo_op,
	.scan =                 NULL,
	.set_trn =              NULL,
	.get_trn =              NULL,
	.decode_event =         NULL,
	.set_channel =          NULL,
	.get_channel =          NULL,

	/* there are some more */
}; 

/* ---------------------------------------------------------------------- */

int ft817_init (RIG *rig)
{
	struct ft817_priv_data *p;
  
	rig_debug (RIG_DEBUG_VERBOSE,"ft817: ft817_init called \n");

	if ((p = calloc(1, sizeof(struct ft817_priv_data))) == NULL)
		return -RIG_ENOMEM;

	/* Copy complete native cmd set to private cmd storage area */
	memcpy(p->pcs, ncmd, sizeof(ncmd));

	rig->state.priv = (void*) p;
  
	return RIG_OK;
}

int ft817_cleanup (RIG *rig)
{
	rig_debug (RIG_DEBUG_VERBOSE,"ft817: ft817_cleanup called \n");

	if (rig->state.priv)
		free(rig->state.priv);
	rig->state.priv = NULL;
  
	return RIG_OK;
}

int ft817_open (RIG *rig)
{
	rig_debug (RIG_DEBUG_VERBOSE,"ft817: ft817_open called \n");

	return RIG_OK;
}

int ft817_close (RIG *rig)
{
	rig_debug (RIG_DEBUG_VERBOSE,"ft817: ft817_close called \n");

	return RIG_OK;
}

/* ---------------------------------------------------------------------- */

static inline long timediff(struct timeval *tv1, struct timeval *tv2)
{
	struct timeval tv;

	tv.tv_usec = tv1->tv_usec - tv2->tv_usec;
	tv.tv_sec  = tv1->tv_sec  - tv2->tv_sec;

	return ((tv.tv_sec * 1000L) + (tv.tv_usec / 1000L));
}

static int check_cache_timeout(struct timeval *tv)
{
	struct timeval curr;
	long t;

	if (tv->tv_sec == 0 && tv->tv_usec == 0) {
		rig_debug(RIG_DEBUG_VERBOSE, "ft817: cache invalid\n");
		return 1;
	}

	gettimeofday(&curr, NULL);

	if ((t = timediff(&curr, tv)) < FT817_CACHE_TIMEOUT) {
		rig_debug(RIG_DEBUG_VERBOSE, "ft817: using cache (%ld ms)\n", t);
		return 0;
	} else {
		rig_debug(RIG_DEBUG_VERBOSE, "ft817: cache timed out (%ld ms)\n", t);
		return 1;
	}
}

static int ft817_get_status(RIG *rig, int status)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
	struct timeval *tv;
	unsigned char *data;
	int len;
	int n;

	switch (status) {
	case FT817_NATIVE_CAT_GET_FREQ_MODE_STATUS:
		data = p->fm_status;
		len  = YAESU_CMD_LENGTH;
		tv   = &p->fm_status_tv;
		break;
	case FT817_NATIVE_CAT_GET_RX_STATUS:
		data = &p->rx_status;
		len  = 1;
		tv   = &p->rx_status_tv;
		break;
	case FT817_NATIVE_CAT_GET_TX_STATUS:
		data = &p->tx_status;
		len  = 1;
		tv   = &p->tx_status_tv;
		break;
	default:
		rig_debug(RIG_DEBUG_ERR, "ft817_get_status: Internal error!\n");
		return -RIG_EINTERNAL;
	}

	serial_flush(&rig->state.rigport);

	write_block(&rig->state.rigport, p->pcs[status].nseq, YAESU_CMD_LENGTH);

	if ((n = read_block(&rig->state.rigport, data, len)) < 0)
		return n;

	if (n != len)
		return -RIG_EIO;

	gettimeofday(tv, NULL);

	return RIG_OK;
}

/* ---------------------------------------------------------------------- */

int ft817_get_freq(RIG *rig, vfo_t vfo, freq_t *freq)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
	int n;

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	if (check_cache_timeout(&p->fm_status_tv))
		if ((n = ft817_get_status(rig, FT817_NATIVE_CAT_GET_FREQ_MODE_STATUS)) < 0)
			return n;

	*freq = from_bcd_be(p->fm_status, 8) * 10;

	return -RIG_OK;
}

int ft817_get_mode(RIG *rig, vfo_t vfo, rmode_t *mode, pbwidth_t *width)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
	int n;

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	if (check_cache_timeout(&p->fm_status_tv))
		if ((n = ft817_get_status(rig, FT817_NATIVE_CAT_GET_FREQ_MODE_STATUS)) < 0)
			return n;

	/* set normal width now, narrow will override this later */
	*width = RIG_PASSBAND_NORMAL;

	switch (p->fm_status[4]) {
	case 0x00:
		*mode = RIG_MODE_LSB;
		break;
	case 0x01:
		*mode = RIG_MODE_USB;
		break;
	case 0x02:
		*mode = RIG_MODE_CW;
		break;
	case 0x03:
		*mode = RIG_MODE_CWR;
		break;
	case 0x04:
		*mode = RIG_MODE_AM;
		break;
	case 0x06:
		*mode = RIG_MODE_WFM;
		break;
	case 0x08:
		*mode = RIG_MODE_FM;
		break;
	case 0x0A:
		*mode = RIG_MODE_RTTY;
		break;
	case 0x0C:
		*mode = RIG_MODE_PKTFM;
		break;

	/* "extra modes" which are not documented in the manual */
	case 0x82:
		*mode = RIG_MODE_CW;
		*width = rig_passband_narrow (rig, RIG_MODE_CW);
		break;
	case 0x83:
		*mode = RIG_MODE_CWR;
		*width = rig_passband_narrow (rig, RIG_MODE_CW);
		break;
	case 0x8A:
		*mode = RIG_MODE_RTTY;
		*width = rig_passband_narrow (rig, RIG_MODE_CW);
		break;

	default:
		*mode = RIG_MODE_NONE;
	}


	return RIG_OK;
}

int ft817_get_ptt(RIG *rig, vfo_t vfo, ptt_t *ptt)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
	int n;

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	if (check_cache_timeout(&p->tx_status_tv))
		if ((n = ft817_get_status(rig, FT817_NATIVE_CAT_GET_TX_STATUS)) < 0)
			return n;

	*ptt = ((p->tx_status & 0x80) == 0);

	return RIG_OK;
}

static int ft817_get_pometer_level(RIG *rig, value_t *val)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
	int n;

	if (check_cache_timeout(&p->tx_status_tv))
		if ((n = ft817_get_status(rig, FT817_NATIVE_CAT_GET_TX_STATUS)) < 0)
			return n;

	/* Valid only if PTT is on */
	if ((p->tx_status & 0x80) == 0)
		val->f = ((p->tx_status & 0x0F) / 15.0);
	else
		val->f = 0.0;

	return RIG_OK;
}

static int ft817_get_smeter_level(RIG *rig, value_t *val)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
	int n;

	if (check_cache_timeout(&p->rx_status_tv))
		if ((n = ft817_get_status(rig, FT817_NATIVE_CAT_GET_RX_STATUS)) < 0)
			return n;

	n = (p->rx_status & 0x0F) - 9;

	val->i = n * ((n > 0) ? 10 : 6);

	return RIG_OK;
}

static int ft817_get_raw_smeter_level(RIG *rig, value_t *val)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
	int n;

	if (check_cache_timeout(&p->rx_status_tv))
		if ((n = ft817_get_status(rig, FT817_NATIVE_CAT_GET_RX_STATUS)) < 0)
			return n;

	val->i = p->rx_status & 0x0F;

	return RIG_OK;
}


int ft817_get_level (RIG *rig, vfo_t vfo, setting_t level, value_t *val)
{
	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	switch (level) {

	case RIG_LEVEL_STRENGTH:
		return ft817_get_smeter_level(rig, val);

	case RIG_LEVEL_RAWSTR:
		return ft817_get_raw_smeter_level(rig, val);

	case RIG_LEVEL_RFPOWER:
		return ft817_get_pometer_level(rig, val);

	default:
		return -RIG_EINVAL;
	}

	return RIG_OK;
}

int ft817_get_dcd(RIG *rig, vfo_t vfo, dcd_t *dcd)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
	int n;

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	if (check_cache_timeout(&p->rx_status_tv))
		if ((n = ft817_get_status(rig, FT817_NATIVE_CAT_GET_RX_STATUS)) < 0)
			return n;

	/* TODO: consider bit 6 too ??? (CTCSS/DCS code match) */
	if (p->rx_status & 0x80)
		*dcd = RIG_DCD_OFF;
	else
		*dcd = RIG_DCD_ON;

	return RIG_OK;
}

/* ---------------------------------------------------------------------- */

static int ft817_read_ack(RIG *rig)
{
#if (FT817_POST_WRITE_DELAY == 0)
	unsigned char dummy;
	int n;

	if ((n = read_block(&rig->state.rigport, &dummy, 1)) < 0) {
		rig_debug(RIG_DEBUG_ERR, "ft817: error reading ack\n");
		return n;
	}

	rig_debug(RIG_DEBUG_TRACE,"ft817: ack received (%d)\n", dummy);

	if (dummy != 0)
		return -RIG_ERJCTED;
#endif

	return RIG_OK;
}

/*
 * private helper function to send a private command sequence.
 * Must only be complete sequences.
 */
static int ft817_send_cmd(RIG *rig, int index)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
 
	if (p->pcs[index].ncomp == 0) {
		rig_debug(RIG_DEBUG_VERBOSE, "ft817: Incomplete sequence\n");
		return -RIG_EINTERNAL;
	}

	write_block(&rig->state.rigport, p->pcs[index].nseq, YAESU_CMD_LENGTH);
	return ft817_read_ack(rig);
}

/*
 * The same for incomplete commands.
 */
static int ft817_send_icmd(RIG *rig, int index, unsigned char *data)
{
	struct ft817_priv_data *p = (struct ft817_priv_data *) rig->state.priv;
	unsigned char cmd[YAESU_CMD_LENGTH];
 
	if (p->pcs[index].ncomp == 1) {
		rig_debug(RIG_DEBUG_VERBOSE, "ft817: Complete sequence\n");
		return -RIG_EINTERNAL;
	}

	cmd[YAESU_CMD_LENGTH - 1] = p->pcs[index].nseq[YAESU_CMD_LENGTH - 1];
	memcpy(cmd, data, YAESU_CMD_LENGTH - 1);

	write_block(&rig->state.rigport, cmd, YAESU_CMD_LENGTH);
	return ft817_read_ack(rig);
}

/* ---------------------------------------------------------------------- */

int ft817_set_freq(RIG *rig, vfo_t vfo, freq_t freq)
{
	unsigned char data[YAESU_CMD_LENGTH - 1];

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE,"ft817: requested freq = %"PRIfreq" Hz\n", freq);

	/* fill in the frequency */
	to_bcd_be(data, (freq + 5) / 10, 8);

	return ft817_send_icmd(rig, FT817_NATIVE_CAT_SET_FREQ, data);
}

int ft817_set_mode(RIG *rig, vfo_t vfo, rmode_t mode, pbwidth_t width)
{
	int index;	/* index of sequence to send */

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE,"ft817: generic mode = %x \n", mode);

	switch(mode) {

	case RIG_MODE_AM:
		index = FT817_NATIVE_CAT_SET_MODE_AM;
		break;

	case RIG_MODE_CW:
		index = FT817_NATIVE_CAT_SET_MODE_CW;
		break;

	case RIG_MODE_USB:
		index = FT817_NATIVE_CAT_SET_MODE_USB;
		break;

	case RIG_MODE_LSB:
		index = FT817_NATIVE_CAT_SET_MODE_LSB;
		break;

	case RIG_MODE_RTTY:
		index = FT817_NATIVE_CAT_SET_MODE_DIG;
		break;

	case RIG_MODE_FM:
		index = FT817_NATIVE_CAT_SET_MODE_FM;
		break;

	case RIG_MODE_WFM:
		/* can not be set, it is implicit when changing band */
//		index = FT817_NATIVE_CAT_SET_MODE_FM;
		return -RIG_EINVAL;
		break;

	case RIG_MODE_CWR:
		index = FT817_NATIVE_CAT_SET_MODE_CWR;
		break;

	case RIG_MODE_PKTFM:
		index = FT817_NATIVE_CAT_SET_MODE_PKT;
		break;

	default:
		return -RIG_EINVAL;
	}

	/* just ignore passband */
/* 	if (width != RIG_PASSBAND_NORMAL) */
/* 		return -RIG_EINVAL; */

	return ft817_send_cmd(rig, index);
}

int ft817_set_ptt(RIG *rig, vfo_t vfo, ptt_t ptt)
{
	int index, n;

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE, "ft817: ft817_set_ptt called\n");

	switch(ptt) {
	case RIG_PTT_ON:
		index = FT817_NATIVE_CAT_PTT_ON;
		break;
	case RIG_PTT_OFF:
		index = FT817_NATIVE_CAT_PTT_OFF;
		break;
	default:
		return -RIG_EINVAL;
	}

	n = ft817_send_cmd(rig, index);

	if (n < 0 && n != -RIG_ERJCTED)
		return n;

	return RIG_OK;
}

int ft817_set_func (RIG *rig, vfo_t vfo, setting_t func, int status)
{
	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	switch (func) {
	case RIG_FUNC_LOCK:
		if (status)
			return ft817_send_cmd (rig, FT817_NATIVE_CAT_LOCK_ON);
		else
			return ft817_send_cmd (rig, FT817_NATIVE_CAT_LOCK_OFF);

	case RIG_FUNC_TONE:
		if (status)
			return ft817_send_cmd (rig, FT817_NATIVE_CAT_SET_CTCSS_ENC_ON);
		else
			return ft817_send_cmd (rig, FT817_NATIVE_CAT_SET_CTCSS_DCS_OFF);

	case RIG_FUNC_TSQL:
		if (status)
			return ft817_send_cmd (rig, FT817_NATIVE_CAT_SET_CTCSS_ON);
		else
			return ft817_send_cmd (rig, FT817_NATIVE_CAT_SET_CTCSS_DCS_OFF);

	default:
		return -RIG_EINVAL;
	}
}

int ft817_set_dcs_code(RIG *rig, vfo_t vfo, tone_t code)
{
	unsigned char data[YAESU_CMD_LENGTH - 1];
/* 	int n; */

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE, "ft817: set DCS code (%d)\n", code);

	if (code == 0)
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_CTCSS_DCS_OFF);

	/* fill in the DCS code - the rig doesn't support separate codes... */
	to_bcd_be(data,     code, 4);
	to_bcd_be(data + 2, code, 4);


	/* FT-817 does not have the DCS_ENC_ON command, so we just set the tone here */

/* 	if ((n = ft817_send_icmd(rig, FT817_NATIVE_CAT_SET_DCS_CODE, data)) < 0) */
/* 		return n; */

/* 	return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_DCS_ENC_ON); */

	return ft817_send_icmd(rig, FT817_NATIVE_CAT_SET_DCS_CODE, data);
}

int ft817_set_dcs_sql (RIG *rig, vfo_t vfo, tone_t code)
{
	unsigned char data[YAESU_CMD_LENGTH - 1];
	int n;

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE, "ft817: set DCS sql (%d)\n", code);

	if (code == 0)
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_CTCSS_DCS_OFF);

	/* fill in the DCS code - the rig doesn't support separate codes... */
	to_bcd_be(data,     code, 4);
	to_bcd_be(data + 2, code, 4);

	if ((n = ft817_send_icmd(rig, FT817_NATIVE_CAT_SET_DCS_CODE, data)) < 0)
		return n;

	return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_DCS_ON);
}


int ft817_set_ctcss_tone (RIG *rig, vfo_t vfo, tone_t tone)
{
	unsigned char data[YAESU_CMD_LENGTH - 1];
	int n;

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE, "ft817: set CTCSS tone (%.1f)\n", tone / 10.0);

	if (tone == 0)
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_CTCSS_DCS_OFF);

	/* fill in the CTCSS freq - the rig doesn't support separate tones... */
	to_bcd_be(data,     tone, 4);
	to_bcd_be(data + 2, tone, 4);

	if ((n = ft817_send_icmd(rig, FT817_NATIVE_CAT_SET_CTCSS_FREQ, data)) < 0)
		return n;

	return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_CTCSS_ENC_ON);
}


int ft817_set_ctcss_sql(RIG *rig, vfo_t vfo, tone_t tone)
{
	unsigned char data[YAESU_CMD_LENGTH - 1];
	int n;

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE, "ft817: set CTCSS sql (%.1f)\n", tone / 10.0);

	if (tone == 0)
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_CTCSS_DCS_OFF);

	/* fill in the CTCSS freq - the rig doesn't support separate tones... */
	to_bcd_be(data,     tone, 4);
	to_bcd_be(data + 2, tone, 4);

	if ((n = ft817_send_icmd(rig, FT817_NATIVE_CAT_SET_CTCSS_FREQ, data)) < 0)
		return n;

	return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_CTCSS_ON);
}

int ft817_set_rptr_shift (RIG *rig, vfo_t vfo, rptr_shift_t shift)
{
	/* Note: this doesn't have effect unless FT817 is in FM mode
	   although the command is accepted in any mode.
	*/
	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE, "ft817: set repeter shift = %i\n", shift);

	switch (shift) {

	case RIG_RPT_SHIFT_NONE:
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_RPT_SHIFT_SIMPLEX);

	case RIG_RPT_SHIFT_MINUS:
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_RPT_SHIFT_MINUS);

	case RIG_RPT_SHIFT_PLUS:
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_RPT_SHIFT_PLUS);

	}

	return -RIG_EINVAL;
}

int ft817_set_rptr_offs(RIG *rig, vfo_t vfo, shortfreq_t offs)
{
	unsigned char data[YAESU_CMD_LENGTH - 1];

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE, "ft817: set repeter offs = %li\n", offs);

	/* fill in the offset freq */
	to_bcd_be(data, offs / 10, 8);

	return ft817_send_icmd(rig, FT817_NATIVE_CAT_SET_RPT_OFFSET, data);
}

int ft817_set_rit(RIG *rig, vfo_t vfo, shortfreq_t rit)
{
	unsigned char data[YAESU_CMD_LENGTH - 1];
	int n;

	if (vfo != RIG_VFO_CURR)
		return -RIG_ENTARGET;

	rig_debug(RIG_DEBUG_VERBOSE, "ft817: set rit = %li)\n", rit);

	/* fill in the RIT freq */
	data[0] = (rit < 0) ? 255 : 0;
	data[1] = 0;
	to_bcd_be(data + 2, labs(rit) / 10, 4);

	if ((n = ft817_send_icmd(rig, FT817_NATIVE_CAT_SET_CLAR_FREQ, data)) < 0)
		return n;

	/* the rig rejects if these are repeated - don't confuse user with retcode */
	if (rit == 0)
		ft817_send_cmd(rig, FT817_NATIVE_CAT_CLAR_OFF);
	else
		ft817_send_cmd(rig, FT817_NATIVE_CAT_CLAR_ON);

	return RIG_OK;
}


int ft817_set_powerstat(RIG *rig, powerstat_t status)
{
	switch (status) {
	case RIG_POWER_OFF:
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_PWR_OFF);
	case RIG_POWER_ON:
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_PWR_ON);
	case RIG_POWER_STANDBY:
	default:
		return -RIG_EINVAL;
	}
}

int ft817_vfo_op         (RIG *rig, vfo_t vfo, vfo_op_t op)
{
	switch (op) {

	case RIG_OP_TOGGLE:
		return ft817_send_cmd(rig, FT817_NATIVE_CAT_SET_VFOAB);

	default:
		return -RIG_EINVAL;
	}
}


/* FIXME: this function silently ignores the vfo args and just turns
   split ON or OFF.
*/
int ft817_set_split_vfo  (RIG *rig, vfo_t vfo, split_t split, vfo_t tx_vfo)
{
	int index, n;

/* 	if (vfo != RIG_VFO_CURR) */
/* 		return -RIG_ENTARGET; */

	rig_debug(RIG_DEBUG_VERBOSE, "ft817: ft817_set_split_vfo called\n");

	switch (split) {

	case RIG_SPLIT_ON:
		index = FT817_NATIVE_CAT_SPLIT_ON;
		break;

	case RIG_SPLIT_OFF:
		index = FT817_NATIVE_CAT_SPLIT_OFF;
		break;

	default:
		return -RIG_EINVAL;
	}

	n = ft817_send_cmd (rig, index);

	if (n < 0 && n != -RIG_ERJCTED)
		return n;

	return RIG_OK;

}


/* ---------------------------------------------------------------------- */

