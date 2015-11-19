/* opcon.c: Interface to a real operator console
 
   Copyright (c) 2006-2015, Edward Groenenberg & Henk Gooijen

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHOR or AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
   OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.

   Except as contained in this notice, the name of the Author or Authors `
   shall not be used in advertising or otherwise to promote the sale, use
   or other dealings in this Software without prior written
   authorization from the Author or Authors.

   14-Mar-14    EG      added rom address range check, changed interval
   10-Mar-14    EG      oc_get_console : add inv addr check 
                        oc_extract_address : address masking & inv addr flag
                        added function oc_get_rotary; fix load adrs cmd not -
                        to generate 'dep pc xxx'.
   10-Feb-14    EG      Rewrite, based on original realcons.c code.
*/

/* 
 * oc_attach()			: attach device (initialize link)
 * oc_detach()			: detach device (close link)
 * oc_extract_address()		: extract address from swr data array
 * oc_extract_data()		: extract data from swr data array
 * oc_get_console()		: get function command
 * oc_get_halt()		: check for HALT key down
 * oc_get_rotary()		: get rotary knob settings
 * oc_get_swr()			: get switch & knob settings from console
 * oc_mmu()			: toggle mmu mapping mode (16/18/22 bit)
 * oc_poll()			: pool input channel for data ready
 * oc_port1()			: toggle bit on/off in port1 flagbyte
 * oc_port2()			: toggle bit on/off in port2 flagbyte
 * oc_read_line_p()		: get command (keyboard or console)
 * oc_reset()			: device reset
 * oc_ringprot()		: toggle ring protection bits (USK)
 * oc_send_address()		: send address data
 * oc_send_address_data()	: combined send address & data
 * oc_send_all()		: send address/data/status
 * oc_send_data()		: send data data
 * oc_send_status()		: send status data
 * oc_show()			: show status of the device (link)
 * oc_svc()			: service routine
 * oc_toggle_ack()		: acknoledge a state on the console processor
 * oc_toggle_clear()		: clear all states on the console processor
 * oc_help()			: Generic help
 * oc_help_attach()		: Help for attach command
 *
 * More information can be found in the doc/opcon_doc.txt file
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include "sim_defs.h"
#include "sim_tmxr.h"
#include "scp.h"
#include "pdp11_defs.h"
#include "opcon.h"
#include <ctype.h>

#include <termio.h>
#include <sys/ioctl.h>

/* Declarations & references for required external routines & variables. */
extern SERHANDLE sim_open_serial (char *name, TMLN *lp, t_stat *status);
extern SERHANDLE sim_close_serial (SERHANDLE port);
extern char *do_position (void);
extern int32 sim_read_serial(SERHANDLE port, char *bufp, int32 count,char *brk);
extern int32 sim_write_serial (SERHANDLE port, char *bufp, int32 count);
extern int32 sim_quiet;			/* Quiet output mode */
extern int32 MMR0, MMR3;		/* MMU registers */
extern UNIT cpu_unit;			/* configured memory size */
extern int32 sim_do_echo;

/* Defines */
#define OC_INTERVAL	     1000	/* # of nanoseconds */

#define INP1			0
#define INP2			1
#define INP3			2
#define INP4			3
#define INP5			4
#define SWR_00_07_PORT	     INP1	/* SWITCH REGISTER 7-0 */
#define SWR_08_15_PORT	     INP2	/* SWITCH REGISTER 15-8 */
#define SWR_16_22_PORT	     INP3	/* SWITCH REGISTER 16-22 */

#define QUERY_SWR_BYTES		5	/* # bytes sent by Query command */

/* 11/05 switches / ports, etc. */
#define SW_PL_1105	     0x80	/* key switch  bitfield */
#define SW_HE_1105	     0x01	/* HALT bitfield */

/* 11/20 switches / ports, etc. */
#define SW_PL_1120	     0x80	/* key switch  bitfield */
#define SW_HE_1120	     0x01	/* HALT bitfield */

/* 11/40 switches / ports, etc. */
#define SW_PL_1140	     0x80	/* key switch  bitfield */
#define SW_HE_1140	     0x01	/* HALT bitfield */

/* 11/45 switches / ports, etc. */
#define SW_PL_1145	     0x80	/* key switch  bitfield */
#define SW_HE_1145	     0x01	/* HALT bitfield */

/* 11/70 switches / ports, etc. */
#define SW_PL_1170	     0x80	/* key switch bitfield */
#define SW_HE_1170	     0x40	/* HALT bitfield */

/* DISPLAY DATA rotary switch for 11/45 & 11/70 */
#define DSPD_BUS_REG	     0x00	/* BUS REG */
#define DSPD_DATA_PATHS	     0x01	/* DATA PATHS */
#define DSPD_DISP_REG	     0x02	/* DISPLAY REGISTER */
#define DSPD_MU_ADRS	     0x03	/* uADRS FPP/CPU */
#define DSPD_MASK	     0x03	/* mask for DSPA range */

/* DISPLAY ADDRESS rotary switch for 11/45 & 11/70 */
#define DSPA_PROGPHY	     0x00	/* PROG PHY */
#define DSPA_KERNEL_D	     0x01	/* KERNEL D */
#define DSPA_KERNEL_I	     0x02	/* KERNEL I */
#define DSPA_CONSPHY	     0x03	/* CONS PHY */
#define DSPA_SUPER_D	     0x04	/* SUPER D */
#define DSPA_SUPER_I	     0x05	/* SUPER I */
#define DSPA_USER_D	     0x06	/* USER D */
#define DSPA_USER_I	     0x07	/* USER I */
#define DSPA_MASK	     0x07	/* mask for DSPA range */

/* Ack_toggle flag definitions */
#define ACK_DEPO	     0x40
#define ACK_CONT	     0x08
#define ACK_LOAD	     0x04
#define ACK_START	     0x02
#define ACK_EXAM	     0x01
#define ACK_MASK	     0x4F

/* Definitions copied from pdp11_defs.h, including it directly causes errors. */
#define MMR0_MME	  0000001	/* 18 bit MMU enabled */
#define MMR3_M22E	      020	/* 22 bit MMU enabled */
#define MD_KER		        0	/* protection mode - KERNEL */
#define MD_SUP		        1	/* protection mode - SUPERVISOR */
#define MD_UND		        2	/* protection mode - UNDEFINED */
#define MD_USR		        3	/* protection mode - USER */

/* Debug levels for the OC device */
#define OCDEB_CON	      001	/* console input */
#define OCDEB_HLT	      002	/* halt switch check */
#define OCDEB_STS	      004	/* status leds update */
#define OCDEB_SWR	      010	/* switch register queries */
#define OCDEB_SVC	      020	/* service calls  */
#define OCDEB_TRC	      040	/* trace calls */
#define OCDEB_UPD	      100	/* address & data leds update */

/* Global declarations for the OC device */
oc_st	oc_ctl;				/* OC device control block */

/* Debug flags & keywords for the OC device */
DEBTAB oc_debug[] = {
    { "CON", OCDEB_CON },		/* used in  oc_get_console */
    { "HLT", OCDEB_HLT },		/* used in oc_get_halt */
    { "STS", OCDEB_STS },		/* used in oc_send_status */
    { "SWR", OCDEB_SWR },		/* used in oc_get_swr & oc_get_rotary */
    { "SVC", OCDEB_SVC },		/* used in oc_svc */
    { "TRC", OCDEB_TRC },		/* used in all major call entry points */
    { "UPD", OCDEB_UPD },		/* used in oc_send_address_data & oc_send_all */
    { 0 }
    };

/* UNIT definition */
UNIT oc_unit = { UDATA (&oc_svc, UNIT_ATTABLE+UNIT_DISABLE+UNIT_DIS, 0) };

/* Modifiers definitions */
MTAB oc_mod[] = { 
    { MTAB_XTD|MTAB_VDV|MTAB_NMO, 0, "STATUS", NULL,
        NULL, &oc_show, NULL, "Display console link status" },
    { 0 } 
    };	

/* DEVICE definition */
DEVICE oc_dev = {
    "OC",					/* device code */
    &oc_unit,					/* UNIT structure */
    NULL,					/* register (N/U) */
    oc_mod,					/* modifier options */
    1,						/* # of units (N/U) */
    0,						/* address radix (N/U) */
    0,						/* address width (N/U) */
    0,						/* address increment (N/U) */
    0,						/* data radix (N/U) */
    0,						/* data width (N/U) */
    NULL,					/* examine function (N/U) */
    NULL,					/* deposit function (N/U) */
    &oc_reset,					/* reset function */
    NULL,					/* boot function (N/U) */
    &oc_attach,					/* attach function */
    &oc_detach,					/* detach function */
    NULL,					/* context (N/U) */
    (DEV_DIS | DEV_DISABLE | DEV_DEBUG),	/* device flags */
    0,						/* debug control (N/U) */
    oc_debug,					/* debug options */
    NULL,					/* memory size function (N/U) */
    NULL,					/* logical name (N/U) */
    &oc_help,					/* help function */
    &oc_help_attach,				/* attach help function */
    NULL,					/* help context (N/U) */
    &oc_description				/* description function */
    };

/* Serial line definition */
TMLN oc_ldsc = { 0 };

/* Multiplexer definition */
TMXR oc_tmxr = { 1, 0, 0, &oc_ldsc, NULL, &oc_dev };

/*
 * Function : oc_attach()
 * Note	    : Attach & activate the console processor
 *	      A request for switch status is executed, this is needed to
 * 	      know the HALT/ENABLE switch position.
 * 	      The halt mode is set to '0' and not '1' as we have not started
 * 	      anything at this point.
 * Returns  : SCPE_OK or -1
 */
t_stat oc_attach (UNIT *uptr, char *cptr)
{
char *cmdp, *tptr;
uint32 H, K;
t_stat r;
SERHANDLE p;

sim_debug (OCDEB_TRC, &oc_dev, "oc_attach : called\n");

oc_ldsc.rcve = FALSE;					/* mark as available */
if (cptr == NULL)
    return SCPE_ARG;

if ((tptr = strchr (cptr, '=')) == NULL)
    return SCPE_ARG;
*tptr++;						/* skip '=' */

if ((p = sim_open_serial (tptr, NULL, &r)) != -1) {	/* port usable? */
    sim_close_serial (p);
    if (r != SCPE_OK)
        return -1;
    }
if ((oc_tmxr.master) || (oc_ldsc.serport))		/* already open? */
    tmxr_close_master (&oc_tmxr);			/* close it first */
if ((r = tmxr_attach (&oc_tmxr, uptr, cptr)))		/* open the link */
    return r;
oc_ldsc.rcve = TRUE;					/* mark as available */

memset (&oc_ctl, 0, sizeof(oc_ctl));		/* init OC control block */
oc_ctl.first_exam = TRUE;
oc_ctl.first_dep = TRUE;

switch (cpu_model) {
    case MOD_1105 : cmdp = "p1"; break;
    case MOD_1120 : cmdp = "p2"; break;
    case MOD_1140 : cmdp = "p3"; break;
    case MOD_1145 : cmdp = "p4"; break;
    case MOD_1170 : cmdp = "p5"; break;
    default       : printf ("OC    : No support for the current cpu model.\n");
		    return SCPE_OK;
    } 

if (sim_write_serial (oc_ldsc.serport, cmdp, 2) != 2) {
    printf ("OC    : Error sending config type to the console\n");
    return -1;
    }

//oc_toggle_clear();			/* clear all toggles */
oc_get_swr ();				/* request console key state */
switch (cpu_model) {			/* detect power & halt key position */
    case MOD_1105 : K = oc_ctl.S[INP2] & SW_PL_1105;
		    H = oc_ctl.S[INP2] & SW_HE_1105; break;
    case MOD_1120 : K = oc_ctl.S[INP2] & SW_PL_1120;
		    H = oc_ctl.S[INP2] & SW_HE_1120; break;
    case MOD_1140 : K = oc_ctl.S[INP2] & SW_PL_1140;
		    H = oc_ctl.S[INP2] & SW_HE_1140; break;
    case MOD_1145 : K = oc_ctl.S[INP3] & SW_PL_1145; 
		    H = oc_ctl.S[INP5] & SW_HE_1145; break;
    case MOD_1170 : K = oc_ctl.S[INP5] & SW_PL_1170;
		    H = oc_ctl.S[INP5] & SW_HE_1170; break;
    }

if (!sim_quiet)
    printf ("OC    : Operator console KEY switch set to ");
if (!K) {
    if (!sim_quiet) {
	printf ("POWER\n");
        printf ("OC    : Operator console ENABLE/HALT switch set to ");
	}
    if (!H) {				/* HALT key is up */
	if (!sim_quiet)
	    printf ("ENABLE\n");
	}
    else {				/* HALT key is down */
	oc_ctl.halt = 2;
	if (!sim_quiet)
	    printf ("HALT\n");
        }
    }
else
    if (!sim_quiet)
	printf ("LOCK\n");

oc_send_all (0x002005, 0x2015);
oc_ctl.resched = sim_os_msec();		/* store initial timer value */

return SCPE_OK;
}

/*
 * Function : oc_detach()
 * Note	    : Deactivate & detach the console processor link
 * Returns  : SCPE_OK or -1
 */
t_stat oc_detach (UNIT *uptr)
{
t_stat r;

if (!oc_ldsc.serport)				/* return if already closed */
    return SCPE_OK;

sim_cancel (&oc_unit);				/* dequeue service routine */

r = tmxr_detach (&oc_tmxr, uptr);		/* detach link */
oc_ldsc.rcve = FALSE;				/* clear receiver flag */

return r;
}

/*
 * Function : oc_reset()
 * Note	    : Reset the device and queue the service routine
 * Returns  : SCPE_OK
 */
t_stat oc_reset (DEVICE *dptr)
{
sim_debug (OCDEB_TRC, &oc_dev, "oc_reset : called\n");
sim_activate_after (&oc_unit, OC_INTERVAL);	/* queue service routine */
return SCPE_OK;
}

/*
 * Function : oc_svc()
 * Note	    : This is the service routine to update the address & data leds.
 *            With a line speed of 9600 bits/s, roughly 800 characters per second
 *            can be transmitted (using the 8N1 setting).
 *
 *   MODE_1 is true
 *            the following actions are executed by the service call:
 *             - send data & address values         (6 bytes per call)
 *               OR
 *               every 5th call :
 *                - send address/data & status      (8 bytes per call)
 *                - check the 'HALT' switch setting (1 byte if set)
 *                - get rotary knobs settings	    (2 bytes per call)
 *
 *   MODE_1 is false
 *            the following actions are executed by the service call:
 *             - sending data,address & status values (8 bytes per call)
 *             - every 5th call :
 *               check the 'HALT' switch setting      (1 byte if set)
 *               get rotary knobs settings	      (2 bytes per call)
 *
 *            updates commence when the simulated processor is running again.
 *
 * Returns  : SCPE_OK
 */
#define MODE_1
#ifdef MODE_1
#define OC_INTERVAL 10
#else
# define OC_INTERVAL 12
#endif

t_stat oc_svc (UNIT *uptr)
{
uint32 A, R;
uint16 D;

if (!oc_ldsc.rcve)			/* console link configured / open? */
    return FALSE;
    
sim_debug (OCDEB_TRC, &oc_dev, "oc_svc : called\n");

R = sim_os_msec();
sim_debug (OCDEB_SVC, &oc_dev, "oc_svc : delta = %d\n", R - oc_ctl.resched);
if ((R - oc_ctl.resched) < OC_INTERVAL) {		/* sufficient time passed by? */
    sim_activate_after (uptr, OC_INTERVAL);	/* reschedule */
    return 0;
    }
oc_ctl.resched = R;			/* set as new marker */

switch (cpu_model) {
    case MOD_1105 :
		D = oc_ctl.D[DISP_SHFR];
		A = oc_ctl.A[ADDR_PRGPA] & 0xFFFF; 
		break;
    case MOD_1120 :
		D = oc_ctl.D[DISP_SHFR];
		A = oc_ctl.A[ADDR_PRGPA] & 0xFFFF; 
		break;
    case MOD_1140 :
		D = oc_ctl.D[DISP_SHFR];
		A = oc_ctl.A[ADDR_PRGPA] & 0x3FFFF;
		break;
    case MOD_1145 : 
		oc_port1(FSTS_1145_INDDATA, oc_ctl.ind_addr);
		switch ((oc_ctl.S[INP3] >> 4) & DSPA_MASK) {
		    case DSPA_PROGPHY : A = oc_ctl.A[ADDR_PRGPA]&0x3FFFF;break;
		    case DSPA_CONSPHY : A = oc_ctl.A[ADDR_CONPA]&0x3FFFF;break;
		    case DSPA_KERNEL_D: A = oc_ctl.A[ADDR_KERND]&0xFFFF; break;
		    case DSPA_KERNEL_I: A = oc_ctl.A[ADDR_KERNI]&0xFFFF; break;
		    case DSPA_SUPER_D : A = oc_ctl.A[ADDR_SUPRD]&0xFFFF; break;
		    case DSPA_SUPER_I : A = oc_ctl.A[ADDR_SUPRI]&0xFFFF; break;
		    case DSPA_USER_D  : A = oc_ctl.A[ADDR_USERD]&0xFFFF; break;
		    case DSPA_USER_I  : A = oc_ctl.A[ADDR_USERI]&0xFFFF; break;
		    }
		switch ((oc_ctl.S[INP3] >> 2) & DSPD_MASK) {
		    case DSPD_DATA_PATHS : D = oc_ctl.D[DISP_SHFR]; break;
		    case DSPD_BUS_REG    : D = oc_ctl.D[DISP_BR];   break;
		    case DSPD_MU_ADRS    : D = oc_ctl.D[DISP_FPP];  break;
		    case DSPD_DISP_REG   : D = oc_ctl.D[DISP_DR];   break;
		    }
		break;
    case MOD_1170 :
		oc_port1(FSTS_1170_INDDATA, oc_ctl.ind_addr);
		switch (oc_ctl.S[INP5] & DSPA_MASK) {
		    case DSPA_PROGPHY : A = oc_ctl.A[ADDR_PRGPA]&0x3FFFFF;break;
		    case DSPA_CONSPHY : A = oc_ctl.A[ADDR_CONPA]&0x3FFFFF;break;
		    case DSPA_KERNEL_D: A = oc_ctl.A[ADDR_KERND]&0xFFFF; break;
		    case DSPA_KERNEL_I: A = oc_ctl.A[ADDR_KERNI]&0xFFFF; break;
		    case DSPA_SUPER_D : A = oc_ctl.A[ADDR_SUPRD]&0xFFFF; break;
		    case DSPA_SUPER_I : A = oc_ctl.A[ADDR_SUPRI]&0xFFFF; break;
		    case DSPA_USER_D  : A = oc_ctl.A[ADDR_USERD]&0xFFFF; break;
		    case DSPA_USER_I  : A = oc_ctl.A[ADDR_USERI]&0xFFFF; break;
		    }
		switch ((oc_ctl.S[INP5] >> 3) & DSPD_MASK) {
		    case DSPD_DATA_PATHS : D = oc_ctl.D[DISP_SHFR]; break;
		    case DSPD_BUS_REG    : D = oc_ctl.D[DISP_BR];   break;
		    case DSPD_MU_ADRS    : D = oc_ctl.D[DISP_FPP];  break;
		    case DSPD_DISP_REG   : D = oc_ctl.D[DISP_DR];   break;
		    }
		break;
    }

#ifdef MODE_1
if (oc_ctl.c_upd++ > 4) {		/* Update threshold reached? */
    oc_ctl.c_upd = 0;
    oc_send_all (A, D);
    if (oc_ctl.c_rot++ > 2) {		/* Rotary threshold reached? */
	oc_ctl.c_rot = 0;
        oc_get_rotary ();
        }
    oc_get_halt ();			/* check for HALT key */
    }
else 
    oc_send_address_data (A, D);
#else
oc_send_all (A, D);
if (oc_ctl.c_upd++ > 4) {		/* Update threshold reached? */
    oc_ctl.c_upd = 0;
    if (oc_ctl.c_rot++ > 2) {		/* Rotary threshold reached? */
	oc_ctl.c_rot = 0;
        oc_get_rotary ();
        }
    oc_get_halt ();			/* check for HALT key */
    }
#endif

sim_activate_after (uptr, OC_INTERVAL);		/* reschedule */

return SCPE_OK;
}

/*
 * Function : oc_show()
 * Note	    : Show the status of the link
 * Returns  : SCPE_OK
 */
t_stat oc_show (FILE *st, UNIT *uptr, int32 val, void *desc)
{
if (oc_ldsc.rcve)
    fputs ("active\n", st);
else
    fputs ("not active\n", st);

return SCPE_OK;
}

/*
 * Function : oc_help()
 * Note     : Help about opcon
 *            Processes 'help oc' (not 'help set oc')
 * Returns  : Nothing
 */
t_stat oc_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, char *cptr)
{
const char *const text =
" OC11 Remote Operator Console processor link\n"
"\n"
" The OC11 is a pseudo driver and is an interface to the core-IO console\n"
" processor which allows an original PDP-11 operator console to control the\n"
" behaviour of SIMH.\n"
" Actual address, data & status information is transmitted and switch\n"
" settings (and knobs) are queried 50 times per second.\n"
;

fprintf (st, "%s", text);
fprint_set_help (st, dptr);
fprint_show_help (st, dptr);
oc_help_attach (st, dptr, uptr, flag, cptr);
return SCPE_OK;
}

/*
 * Function : oc_help_attach()
 * Note     : Help about opcon attach
 *            Processes 'help oc' (not 'help set oc')
 * Returns  : Nothing
 */
t_stat oc_help_attach (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, char *cptr)
{
const char *const text =
" OC device ATTACH help."
"\n"
" The OC driver uses a single serial port to send and receive commands"
" and data to and from the console processor."
"\n"
" The ATTACH command specifies which serial port to be used.\n"
" A serial port may be specified as an operating system specific device name\n"
" or useing simh generic serial name. Simh generica names are of the form\n"
" serN, where N is from 0 thru one less than the maximum number of serial\n"
" ports on the local system. The mapping of simh generic port names to OS \n"
" specific names can be displayed using the following command:\n"
"\n"
"   sim> SHOW SERIAL\n"
"   Serial devices:\n"
"    ser0   /dev/ttyS0\n"
"    ser1   /dev/ttyS1\n"
"\n"
"   sim> ATTACH OC connect=ser0\n"
"\n"
" or equivalently:\n"
"\n"
"   sim> ATTACH OC connect=/dev/ttyS1\n"
"\n"
" Valid port numbers are from 0 thru 31\n"
"\n"
" An optional serial port configuration string may be present after the port\n"
" name.  If present, it must be separated from the port name with a semicolon\n"
" and has this form:\n"
"\n"
"   <rate>-<charsize><parity><stopbits>\n"
"\n"
" where:\n"
"   rate     = communication rate in bits per second\n"
"   charsize = character size in bits (5-8, including optional parity)\n"
"   parity   = parity designator (N/E/O/M/S for no/even/odd/mark/space parity)\n"
"   stopbits = number of stop bits (1, 1.5, or 2)\n"
"\n"
" As an example:\n"
"\n"
"   9600-8n1\n"
" The supported rates, sizes, and parity options are host-specific. If\n"
" a configuration string is not supplied, then the default of 9600-8N1\n"
" is used.\n"
"\n"
" The connection configured for the OC device are unconfigured by:\n"
"\n"
"   sim> DETACH OC\n"
"\n"
" This will  disable any communication to the console processor as well.\n"
"\n"
;

fprintf (st, "%s", text);
fprint_set_help (st, dptr);
fprint_show_help (st, dptr);
return SCPE_OK;
}

/*
 * Function : oc_description()
 * Note     : Single line description
 * Returns  : Pointer to the text
 */
char *oc_description (DEVICE *dptr)
{
return "OC11 : Interface to operator console processor";
}

/*
 * Function : oc_get_console()
 * Note     : Poll the console link for a single byte command.
 * 	      It is processed and appropiate action is taken.
 *
 *            There are special address increment conditions when depositing
 *            data in the register area, just +1 iso +2 for the general
 *            register range R0 [777 700] thru R7 [777 707].
 *
 *            Deposits in the boot rom address range is not allowed, the
 *            range is from 165000 - 167000 & 173000 - 174000
 *            (17765000 - 17767000 & 1777300 - 17774000)
 *            The 'load address' returns a dummy command as it only pre-sets
 *            the active address field in the controlblock.
 *
 * Returns  : FALSE when there is no console pseudo command available
 * 		    exception (as with a real console) is the HALT switch
 *            TRUE  when a pseudo command is returned
 */
t_bool oc_get_console (char *cptr)
{
char brk = 0;
uint8 c = 0;
uint16 D;
uint32 A;

sim_debug (OCDEB_TRC, &oc_dev, "oc_get_console : called\n");

if (!oc_ldsc.rcve)			/* console link configured / open? */
    return FALSE;

if (oc_poll (oc_ldsc.serport, 10000) != 1 ||	/* wait 10 msec for input */
    sim_read_serial (oc_ldsc.serport, &c, 1, &brk) != 1 || 
    c == 0)
    return FALSE;

sim_debug (OCDEB_CON, &oc_dev, "oc_get_console : byte = 0x%02X (%c)\n", c, c);

switch (c) {
    case 'H' :					/* HALT/ENABLE->HALT */
	oc_ctl.halt = 2;
	strcpy (cptr, ";halt key down\n");
	break;
    case 'E' :					/* HALT/ENABLE->ENABLE */
	oc_ctl.halt = 1;
	strcpy (cptr, ";halt key up\n");
//	oc_toggle_ack (ACK_CONT);
	oc_toggle_clear ();
	break;
    case 'c' :					/* CONTINUE */
	oc_toggle_ack (ACK_CONT);
	if (oc_ctl.halt == 2)
	    strcpy (cptr, "step\n");		/* STEP when HALT is down */
	else {
	    strcpy (cptr, "continue\n");
	    if (cpu_model == MOD_1145)
	        oc_port1 (FSTS_1145_ADRSERR, 0);
	    if (cpu_model == MOD_1170)
	        oc_port1(FSTS_1170_ADRSERR, 0);
	    oc_clear_halt ();
	    }
	break;
    case 'd' :					/* DEPOSIT */
        oc_get_swr ();	/* get actual switch settings */
	if (oc_ctl.first_dep == FALSE) {	/* 1st dep cmd? */
	    if ((oc_ctl.act_addr >= 0x3FFC0) && 
		(oc_ctl.act_addr <= 0x3FFC7))
		oc_ctl.act_addr += 1;	/* in CPU register space +1 */
	    else {
		oc_ctl.act_addr += 2;	/* rest of address space +2 */
		if (oc_ctl.act_addr > 0x3FFFFE) /* 22 bit overflow? */
		    oc_ctl.act_addr = 0;
		if ((oc_ctl.act_addr & 1) != 0)/* make odd adr even */
		    oc_ctl.act_addr &= 0x3FFFFE;
		}
	    }
	if (oc_ctl.inv_addr) {	/* above mem range? */
	    switch (cpu_model) {
		case MOD_1105 : stop_cpu = 1; break; /* should freeze cpu */
		case MOD_1120 : stop_cpu = 1; break; /* should freeze cpu */
		case MOD_1140 : 	      break; /* TBD */
		case MOD_1145 : oc_port1 (FSTS_1145_ADRSERR, 1); break;
		case MOD_1170 : oc_port1 (FSTS_1170_ADRSERR, 1); break;
		}
	    strcpy (cptr, ";address out of defined range\n");
	    }
	else {	/* no deposits in boot rom address range or device roms */
	    if ((((oc_ctl.act_addr & 0x0003FFFF) >= 0x3FEA00) &&  /*17765000 */
	         ((oc_ctl.act_addr & 0x0003FFFF) <  0x3FEC00)) ||
		(((oc_ctl.act_addr & 0x0003FFFF) >= 0xEA00) &&	  /*  165000 */
	         ((oc_ctl.act_addr & 0x0003FFFF) <  0xEC00)) ||
		(((oc_ctl.act_addr & 0x0003FFFF) >= 0x3FF600) &&  /*17775000 */
	         ((oc_ctl.act_addr & 0x0003FFFF) <  0x3FF800)) ||
		(((oc_ctl.act_addr & 0x0003FFFF) >= 0xF600) &&	  /*  175000 */
	         ((oc_ctl.act_addr & 0x0003FFFF) <  0xF800))) {
		strcpy (cptr, ";no deposit in boot rom range\n");
		}
	    else {
		D = oc_extract_data ();		/* get 'data' data */
		oc_ctl.first_exam = TRUE;
		oc_ctl.first_dep = FALSE;
		oc_send_address_data (oc_ctl.act_addr, D);
		sprintf (cptr, "deposit %o %o\n", oc_ctl.act_addr, D);
		}
	    }
	oc_toggle_ack (ACK_DEPO);		/* ack */
	break;
    case 'l' :					/* LOAD ADDRS */
	switch (cpu_model) {	/* clear some flags */
	    case MOD_1105 : break;
	    case MOD_1120 : break; 
	    case MOD_1140 : break;
	    case MOD_1145 : oc_port1 (FSTS_1145_ADRSERR, 0); break;
	    case MOD_1170 : oc_port1 (FSTS_1170_ADRSERR, 0); break;
	    }
        oc_get_swr ();	/* get actual switch settings */
	oc_ctl.first_dep = TRUE;
	oc_ctl.first_exam = TRUE;
	oc_ctl.act_addr = oc_extract_address ();
	oc_send_address (oc_ctl.act_addr);
	sprintf (cptr, ";load address %08o\n", oc_ctl.act_addr);
	oc_toggle_ack (ACK_LOAD) ;
	break;
    case 's' :					/* START */
	if (oc_ctl.halt == 2) {
	    strcpy (cptr, "reset all\n");	/* RESET when HALT is down */
	    if (cpu_model == MOD_1170)
		oc_port1 (FSTS_1170_ADRSERR, 0);
	    }
	else
	    sprintf (cptr, "run %o\n", oc_ctl.act_addr);
//	oc_toggle_ack (ACK_START);
	oc_clear_halt ();
	break;
    case 'x' :					/* EXAMINE */
	if (oc_ctl.first_exam == FALSE) { /* not 1st EXAM: auto-incr. */
	    if ((oc_ctl.act_addr >= 0x3FFC0) && 
		(oc_ctl.act_addr <= 0x3FFC7))
	        oc_ctl.act_addr += 1;	/* in CPU register space +1 */
	    else {
	        oc_ctl.act_addr += 2;	/* rest of address space +2 */
	        if (oc_ctl.act_addr > 0x3FFFFE) /* 22-bit overflow? */
		    oc_ctl.act_addr = 0;
	        if ((oc_ctl.act_addr & 1) != 0)	/* make odd adr even */
		    oc_ctl.act_addr &= 0x3FFFFE;
	        }
	    }
	if (oc_ctl.inv_addr) {
	    switch (cpu_model) {
		case MOD_1105 : stop_cpu = 1; break;
		case MOD_1120 : stop_cpu = 1; break;
		case MOD_1140 : break; /* TBD */
		case MOD_1145 : oc_port1 (FSTS_1145_ADRSERR, 1); break;
		case MOD_1170 : oc_port1 (FSTS_1170_ADRSERR, 1); break;
		}
	    strcpy (cptr, ";address out of defined range\n");
	    }
	else {
	    oc_ctl.first_exam = FALSE;
	    oc_ctl.first_dep = TRUE;
	    oc_send_address (oc_ctl.act_addr);
	    sprintf (cptr, "examine %o\n", oc_ctl.act_addr); 
	    }
	oc_toggle_ack (ACK_EXAM);
	break;
    default :				/* stray byte? just ignore it */
	return FALSE;
	break;
    }

oc_send_status ();			/* update console status leds */
return TRUE;
}

/*
 * Function : oc_extract_address()
 * Note     : Get 3 bytes (up to 22 bit switche information as ADDRESS) and
 *            convert it into a 32 bit unsigned integer.
 * 	      A mask is applied for the target cpu address range.
 *            An address in the I/O page is an allowed range even if the
 *            memory is sized to a lower value
 * Returns  : Unsigned long containing the (masked) address switch data
 */
uint32 oc_extract_address ()
{
uint32 A;

A = oc_ctl.S[SWR_16_22_PORT] * 65536 +
    oc_ctl.S[SWR_08_15_PORT] * 256   +
    oc_ctl.S[SWR_00_07_PORT];

oc_ctl.inv_addr = FALSE;
switch (cpu_model) {		/* I/O page is not out of address range */
    case MOD_1105 :
    case MOD_1120 : A &= 0x0000FFFF; 		/* max 64Kb */
		    if (A >= MEMSIZE && !(A > 0xDFFF && A < 0xFFFF))
			oc_ctl.inv_addr = TRUE;	
		    break;
    case MOD_1140 :
    case MOD_1145 : A &= 0x0003FFFF; 		/* max 256Kb */
		    if (A >= MEMSIZE && !(A > 0x3DFFF && A < 0x3FFFF) )
			oc_ctl.inv_addr = TRUE;	
		    break;
    case MOD_1170 : A &= 0x003FFFFF;       	/* max 4Mb */
		    if (A >= MEMSIZE && !(A > 0x3FDFFF && A < 0x3FFFFF))
			oc_ctl.inv_addr = TRUE;	
		    break;
    }

return A;
}

/*
 * Function : oc_extract_data()
 * Note     : Get 3 bytes (16 bit switches information as DATA) and convert it
 * 	      into a 16 bit unsigned integer
 * Returns  : Binary value of data switch settings 
 */
uint16 oc_extract_data ()
{
return (oc_ctl.S[SWR_08_15_PORT] * 256 + oc_ctl.S[SWR_00_07_PORT]);
}

/*
 * Function : oc_port1()
 * Note     : Toggle a single bit for port1
 * Returns  : Nothing
 */
void oc_port1 (uint8 flag, t_bool action)
{
if (!action)
    oc_ctl.port1 = (oc_ctl.port1 & (~flag));
else
    oc_ctl.port1 = (oc_ctl.port1 | flag);
}

/*
 * Function : oc_port2()
 * Note     : Toggle a single bit for port1
 * Returns  : Nothing
 */
void oc_port2 (uint8 flag, t_bool action)
{
if (!action)
    oc_ctl.port2 = (oc_ctl.port2 & (~flag));
else
    oc_ctl.port2 = (oc_ctl.port2 | flag);
}

/*
 * Function : oc_mmu()
 * Note     : Set the 16/18/22 bit or VIRTUAL or off on the console.
 * Returns  : Nothing
 */
void oc_mmu (void)
{
uint8 status, map = 16;

switch (cpu_model) {
    case MOD_1105 : break;
    case MOD_1120 : break;
    case MOD_1140 : oc_port1 (FSTS_1140_VIRTUAL, 0);
		    break;
    case MOD_1145 : break;
    case MOD_1170 : oc_port2 (FSTS_1170_16BIT, 0);
		    oc_port2 (FSTS_1170_18BIT, 0);
		    oc_port2 (FSTS_1170_22BIT, 0);
		    break;
    }
	/* determine mapping from current processor state */
if (MMR0 & MMR0_MME) {
    map = 18;
    if (MMR3 & MMR3_M22E)
        map = 22;
    }

switch (cpu_model) {
    case MOD_1105 : break;
    case MOD_1120 : break;
    case MOD_1140 : if (map == 18)
			oc_port1 (FSTS_1140_VIRTUAL, 1);
		    break;
    case MOD_1145 : break;
    case MOD_1170 : switch (map) {
			case 16 : oc_port2 (FSTS_1170_16BIT, 1); break;
			case 18 : oc_port2 (FSTS_1170_18BIT, 1); break;
			case 22 : oc_port2 (FSTS_1170_22BIT, 1); break;
			}
		    break;
    }
}

/*
 * Function : oc_ringprot()
 * Note     : Manage the ring protection leds on the console
 * 	      11/40 : *	      USER / VIRTUAL LED
 * 	      11/45 & 11/70 : KERNEL, SUPER and USER *LEDs* are coded in
 *                            2 bits on the console
 * 	      hardware, modes 
 * 	        "00" - KERNEL LED on
 * 	        "01" - SUPER LED on
 *              "11" - USER LED on
 *              "10" - illegal combination
 * Returns  : Nothing
 */
void oc_ringprot (int value)
{
uint8 status;

switch (cpu_model) {
    case MOD_1105 : break;
    case MOD_1120 : break;
    case MOD_1140 : if (value == MD_KER) {
			oc_port1 (FSTS_1140_VIRTUAL, 1);
			oc_port1 (FSTS_1140_USER, 0);
			}
		    else {
			oc_port1 (FSTS_1140_VIRTUAL, 0);
			oc_port1 (FSTS_1140_USER, 1);
			}
		    break;
    case MOD_1145 :
    case MOD_1170 : status = oc_ctl.port1 | 0x03;
		    if (value == MD_KER) status &= 0xFC;
		    if (value == MD_SUP) status &= 0xFD;
		    if (value == MD_USR) status &= 0xFF;
		    oc_ctl.port1 = status;
		    break;
    }
}

/*
 * Function : oc_send_status()
 * Note     : Send 8 or 16 bit function/status for those LEDs to the console.
 * 	      This function keeps track of which LEDs are on/off and maintains
 * 	      their relation to prevent contradictions. For example, the LEDs
 * 	      "CONSOLE" and "RUN" are mutually exclusive.
 * 	      1 status byte for the 11/40, 2 for the 11/45 & 11/70
 * Returns  : Nothing
 */
void oc_send_status ()
{
uint8 buf[4];

if (!oc_ldsc.rcve)
    return;

sim_debug(OCDEB_STS, &oc_dev, "oc_send_status : raw byte1 0x%X, byte2 : 0x%X\n",
        oc_ctl.port1, oc_ctl.port2);

buf[0] = 'F';					/* cmd to use */
buf[1] = oc_ctl.port1;
buf[2] = oc_ctl.port2;

if (sim_write_serial (oc_ldsc.serport, buf, 3) != 3)
    printf("OC    : Error sending STATUS to the console\n");
}

/*
 * Function : oc_send_address()
 * Note     : Send 22 bit information for the ADDRESS LEDs to the real console
 * 	      ADDRESS Register displays the address of data just examined or
 * 	      deposited. During a programmed HALT or WAIT instruction, the 
 * 	      display shows the next instruction address.
 * Returns  : Nothing
 */
void oc_send_address(uint32 A)
{
uint8 mask = 0, buf[4];
int r;

if (!oc_ldsc.rcve)
    return;

sim_debug(OCDEB_TRC, &oc_dev, "oc_send_addr : raw address %06X\n", A);

if (MMR0 & MMR0_MME) {
    mask = 0x03;
    if (MMR3 & MMR3_M22E)
        mask = 0x3F;
    }

buf[0] = 'A';
buf[1] = (uint8)((A >> 16) & mask);
buf[2] = (uint8)((A >>  8) & 0xFF);
buf[3] = (uint8) (A & 0xFF);

if ((r = sim_write_serial(oc_ldsc.serport, buf, 4)) != 4)
    printf("OC    : Error sending ADDRESS to the console (e=%d, wr=%d)\n",
	 errno, r);
}

/*
 * Function : oc_send_data()
 * Note     : Display current data on the operator console.
 * Returns  : Nothing
 */
void oc_send_data(uint16 D)
{
uint8 buf[3];
int r;

if (!oc_ldsc.rcve)
    return;

sim_debug(OCDEB_TRC, &oc_dev, "oc_send_data : raw data : %04X\n", D);

buf[0] = 'D';
buf[1] = (uint8)((D >> 8) & 0xFF);
buf[2] = (uint8) (D & 0xFF);

if ((r = sim_write_serial(oc_ldsc.serport, buf, 3)) != 3)
    printf("OC    : Error sending DATA to the console (e=%d, wr=%d)\n",
	 errno, r);
}

/*
 * Function : oc_send_address_data()
 * Note     : Display current address/data on the operator console.
 * Returns  : Nothing
 */
void oc_send_address_data(uint32 A, uint16 D)
{
uint8 buf[7];
int r;
uint8 mask = 0;			/* 0x00 -> 16b, 0x03 -> 18b, 0x3F -> 22b */

if (!oc_ldsc.rcve)
    return;

sim_debug(OCDEB_UPD, &oc_dev, "oc_send_addr_dat : A:0x%06X D:0x%04X\n", A, D);

if (MMR0 & MMR0_MME) {
    mask = 0x03;
    if (MMR3 & MMR3_M22E)
        mask = 0x3F;
    }

buf[0] = 'B';
buf[1] = (uint8)((A >> 16) & mask); 
buf[2] = (uint8)((A >>  8) & 0xFF);
buf[3] = (uint8) (A & 0xFF);
buf[4] = (uint8)((D >> 8) & 0xFF);
buf[5] = (uint8) (D & 0xFF);

if ((r = sim_write_serial(oc_ldsc.serport, buf, 6)) != 6)
    printf("OC    : Error sending ADDRES & DATA to the console (e=%d, wr=%d)\n",
	 errno, r);
}

/*
 * Function : oc_send_all()
 * Note     : Display current address/data/status on the operator console.
 * Returns  : Nothing
 */
void oc_send_all(uint32 A, uint16 D)
{
uint8 buf[10];
int r;
uint8 mask = 0;

if (!oc_ldsc.rcve)
    return;

sim_debug(OCDEB_UPD, &oc_dev, "oc_send_all : A:0x%06X D:0x%04X\n", A, D);

if (MMR0 & MMR0_MME) {
    mask = 0x03;
    if (MMR3 & MMR3_M22E)
        mask = 0x3F;
    }

buf[0] = 'U';
buf[1] = (uint8)((A >> 16) & mask); 
buf[2] = (uint8)((A >>  8) & 0xFF);
buf[3] = (uint8) (A & 0xFF);
buf[4] = (uint8)((D >>  8) & 0xFF);
buf[5] = (uint8) (D & 0xFF);
buf[6] = oc_ctl.port1;
buf[7] = oc_ctl.port2;

if ((r = sim_write_serial(oc_ldsc.serport, buf, 8)) != 8)
    printf("OC    : Error sending ADDRESS/DATA & STATUS to the console (e=%d, wr=%d)\n",
	 errno, r);
}

/*
 * Function : oc_get_swr()
 * Note     : Send the Query command to the operator console 
 *	      Then read the amount of bytes representing the status of all
 *	      the switches on the operator console, stored in the array
 *  	      "oc_ctl.S[]".
 *  Returns : 0 or -1
 */
int oc_get_swr (void)
{
char brk = 0;
uint8 c = 'Q';
uint32 K;
int x;

if (!oc_ldsc.rcve)
    return 0;

sim_debug (OCDEB_TRC, &oc_dev, "oc_get_swr : called\n");

if (sim_write_serial (oc_ldsc.serport, &c, 1) != 1) {
    printf("OC    : Error sending 'QUERY' command.\n");
    return -1;
    }

	/* retrieve the input port data from the console processor */
for (x = 0; x < QUERY_SWR_BYTES; x++) {
    while (1) { if (oc_poll (oc_ldsc.serport, 1000) == 1) break; }
    c = 0;
    if (sim_read_serial (oc_ldsc.serport, &c, 1, &brk) != 1 &&
        	errno != EAGAIN && errno != EWOULDBLOCK)
        return -1;
    oc_ctl.S[x] = c;
    }

sim_debug (OCDEB_SWR, &oc_dev,
	"oc_get_swr : swreg bytes = 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
	oc_ctl.S[0], oc_ctl.S[1], oc_ctl.S[2], oc_ctl.S[3], oc_ctl.S[4]);

return 0;
}

/*
 * Function : oc_get_rotary()
 * Note     : Send the Rotary command to the operator console 
 *	      Then read the byte representing the status of the 2 rotary knobs.
 *  	      The result is stored in one of the "oc_ctl.S[]" fields matching
 *            the positionof the 'Q' command.
 *  	      This function only works for the 11/45 & 11/70
 *  Returns : 0 or -1
 */
int oc_get_rotary (void)
{
char brk = 0;
uint8 c = 'R';

if (!oc_ldsc.rcve)
    return 0;

sim_debug (OCDEB_TRC, &oc_dev, "oc_get_rotary : called\n");

if (cpu_model != MOD_1145 && cpu_model != MOD_1170)
    return 0;

if (sim_write_serial (oc_ldsc.serport, &c, 1) != 1) {
    printf("OC    : Error sending 'ROTARY' command.\n");
    return -1;
    }

while (1) { if (oc_poll (oc_ldsc.serport, 1000) == 1) break; }
if (sim_read_serial (oc_ldsc.serport, &c, 1, &brk) != 1 &&
	errno != EAGAIN && errno != EWOULDBLOCK)
    return -1;

sim_debug (OCDEB_SWR, &oc_dev, "oc_get_rotary : byte = 0x%02X\n", c);

if (cpu_model == MOD_1145)
    oc_ctl.S[INP3] = (uint32)c;
else
    oc_ctl.S[INP5] = (uint32)c;

return 0;
}

/*
 * Function : oc_get_halt()
 * Note     : Check non-blocking if the HALT/ENABLE switch is set to HALT.
 *            If it is another command byte, only acknowledge it to the
 *            console processor.
 *	      also preempts the read queue as a side effect.
 * Returns  : 0 - nothing
 * 	      1 - set
 */
t_bool oc_get_halt ()
{
char brk = 0;
uint8 c = 0;

if (!oc_ldsc.rcve)
    return FALSE;

sim_debug (OCDEB_TRC, &oc_dev, "oc_get_halt : called\n");

if (sim_read_serial (oc_ldsc.serport, &c, 1, &brk) != 1 || c == 0)
    return FALSE;

sim_debug (OCDEB_HLT, &oc_dev, "oc_get_halt : got (%2X:%c)\n", c, c);

if (c == 'H') {				/* HALT switch down? */
    oc_ctl.halt = 2;			/* flag it */
    return TRUE;
    }
else {
    if (strchr ("cdlsx", c) != NULL)     /* known toggle? -> clear */
	oc_toggle_clear ();
    }

return FALSE;
}

/*
 * Function : oc_toggle_ack()
 * Note     : Send the clear-toggle command to the operator console
 * Returns  : Nothing
 */
void oc_toggle_ack (uint8 mask)
{
uint8 buf[3];
int r;
char *msg;

if (!oc_ldsc.rcve)
    return;

sim_debug (OCDEB_TRC, &oc_dev, "oc_toggle_ack : called, mask = %d\n", mask);

#ifdef DEBUG_OC
switch (mask) {
    case ACK_LOAD  : msg = "clear LOAD request sent to console\n";	break;
    case ACK_EXAM  : msg = "clear EXAM request sent to console\n";	break;
    case ACK_DEPO  : msg = "clear DEP request sent to console\n";	break;
    case ACK_CONT  : msg = "clear CONT request sent to console\n";	break;
    case ACK_START : msg = "clear START request sent to console\n";	break;
    }
sim_debug (OCDEB_TRC, &oc_dev, msg);
#endif

/*
 * port number where toggles are defined  ** IMPLEMENTATION SPECIFIC
 */
buf[0] = 'c';
switch (cpu_model) {
    case MOD_1105 : buf[1] = (uint8)(INP3 + 0x30); break;
    case MOD_1120 : buf[1] = (uint8)(INP3 + 0x30); break;
    case MOD_1140 : buf[1] = (uint8)(INP3 + 0x30); break;
    case MOD_1145 : buf[1] = (uint8)(INP3 + 0x30); break;
    case MOD_1170 : buf[1] = (uint8)(INP3 + 0x30); break;
    }
buf[2] = mask;

if ((r = sim_write_serial (oc_ldsc.serport, buf, 3)) != 3)
    printf ("OC    : Error sending CLEAR_TOGGLE to the console (err=%d,r=%d)\n",
	 errno, r);
}

/*
 * Function : oc_toggle_clear()
 * Note     : Send the clear-ALL-toggles command to the REAL CONSOLE
 * Returns  : Nothing
 */
void oc_toggle_clear (void)
{
uint8 c = 'i';

if (!oc_ldsc.rcve)
    return;

sim_debug (OCDEB_TRC, &oc_dev, "oc_toggle_clear : called\n");

if (sim_write_serial (oc_ldsc.serport, &c, 1) != 1)
    printf("OC    : Error sending CLEAR_ALL_TOGGLES to the console\n");
}

/*
 * Function : oc_read_line_p()
 * Note     : Substitution for the 'read_line_p' function
 *	      A complete command can come from two sources: keyboard or
 *	      console. If a complete command is received from the operator
 *	      console, it is returned immediately. Keystrokes received from
 *	      the keyboard are stored until a 'CR' or 'LF' is received.
 *	      When either source has collected a complete command, this
 *	      function returns the pointer to the received string.
 * Returns  : Pointer to received string
 */
char *oc_read_line_p (char *prompt, char *cptr, int32 size, FILE *stream)
{
char *tptr = cptr, key_entry = 0, brk = 0;

sim_debug (OCDEB_TRC, &oc_dev, "oc_read_line_p : called\n");

if (prompt)
    printf ("%s", sim_prompt); fflush(stdout);

for (;;) {
    oc_master (1);
    if (oc_get_console (cptr) == TRUE) { /* poll console for data */
        printf ("%s", cptr);
        break;
        }

    if (oc_poll (0, 10000) == 1 &&		/* wait for data on keyboard */
        sim_read_serial (0, &key_entry, 1, &brk) > 0) {
        if (key_entry == '\b' && *tptr > *cptr) {	/* backspace */
            *tptr--;
            sim_write_serial (1, &key_entry, 1);	
            sim_write_serial (1, " ", 1);
            sim_write_serial (1, &key_entry, 1);
            }
        else {					/* regular character */
            *tptr++ = key_entry;		/* store the character */
            sim_write_serial (1, &key_entry, 1);/* echo the entry to crt */
            if ((key_entry == '\n') || (key_entry == '\r'))
                break;
            }
        }
    } 	

for (tptr = cptr; tptr < (cptr + size); tptr++) {	/* remove cr or nl */
    if ((*tptr == '\n') || (*tptr == '\r') ||
        (tptr == (cptr + size - 1))) {
        *tptr = 0;
        break;
        }
    }

while (isspace (*cptr))
    cptr++;			/* absorb spaces */

if (*cptr == ';') {
    if (sim_do_echo)		/* echo comments if -v */
	printf ("%s> %s\n", do_position(), cptr);
    if (sim_do_echo && sim_log)
	fprintf (sim_log, "%s> %s\n", do_position(), cptr);
    *cptr = 0;
    }

oc_master (0);
if (oc_ctl.halt == 1)		/* no stray mode flag */
    stop_cpu = 1;

return cptr;			/* points to begin of cmd or 0*/
}

/*
 * Function : oc_halt_status()
 * Note     : Check the value of the oc_ctl.halt variable
 * Returns  : 0 - if halt mode = 0 or 1
 *          : 1 - if halt mode = 2
 */
int oc_halt_status (void)
{
if (oc_ctl.halt == 2) return(1);
return(0);
//return oc_ctl.halt;
}

/*
 * Function : oc_clear_halt()
 * Note     : Clear the halt bit in the swr array & clear all toggles
 * Returns  : Nothing
 */
void oc_clear_halt (void)
{
switch (cpu_model) {
    case MOD_1105 : oc_ctl.S[INP2] = oc_ctl.S[INP2] & (~SW_HE_1105);	break;
    case MOD_1120 : oc_ctl.S[INP3] = oc_ctl.S[INP3] & (~SW_HE_1120);	break;
    case MOD_1140 : oc_ctl.S[INP3] = oc_ctl.S[INP3] & (~SW_HE_1140);	break;
    case MOD_1145 : oc_ctl.S[INP4] = oc_ctl.S[INP4] & (~SW_HE_1145);	break;
    case MOD_1170 : oc_ctl.S[INP4] = oc_ctl.S[INP4] & (~SW_HE_1170);	break;
    }
oc_ctl.halt = 0;
oc_toggle_clear ();
}

/*
 * Function : oc_master()
 * Note     : Set the status to be 'MASTER/PROC' or not.
 *            Used to toggle the MASTER / PROC flag
 * Returns  : Nothing
 */
void oc_master (t_bool flag)
{
switch (cpu_model) {
    case MOD_1105 :					break;
    case MOD_1120 : oc_port1 (FSTS_1120_PROC, flag);	break;
    case MOD_1140 : oc_port1 (FSTS_1140_PROC, flag);	break;
    case MOD_1145 : oc_port1 (FSTS_1145_MASTER, flag);	break;
    case MOD_1170 : oc_port1 (FSTS_1170_MASTER, flag);	break;
    }
}

/*
 * Function : oc_wait()
 * Note     : Set the status to be bus master or not.
 *            Used to toggle the master / proc flag
 * Returns  : Nothing
 */
void oc_wait (t_bool flag)
{
switch (cpu_model) {
    case MOD_1105 : break;
    case MOD_1120 : oc_port1 (FSTS_1120_BUS, flag);		break;
    case MOD_1140 : oc_port1 (FSTS_1140_BUS, flag);		break;
    case MOD_1145 : oc_port1 (FSTS_1145_PAUSE, !flag);
		    oc_port1 (FSTS_1145_RUN, flag);
		    break;
    case MOD_1170 : oc_port1 (FSTS_1170_PAUSE, !flag);
		    oc_port1 (FSTS_1170_RUN, flag);
		    break;
    }
}

/*
 * Function : oc_poll()
 * Note     : Poll a channel with a timeout
 * Returns  : 0 : no error
 *            1 : hit detected
 */
#ifdef _WIN32

t_bool oc_poll (SERHANDLE channel, DWORD p)
{
if (WaitForSingleObject (channel, p / 1000) = WAIT_OBJECT_0) 
    return TRUE;

return FALSE;
}

//#elif defined (__unix__) || (__linux) || (__solaris)
#else 

t_bool oc_poll (int channel, int p)
{
fd_set s;
struct timeval t;

t.tv_sec = 0;
t.tv_usec = p;

FD_ZERO (&s);
FD_SET (channel, &s);

select (FD_SETSIZE, &s, NULL, NULL, &t);

if (FD_ISSET (channel, &s))
    return TRUE;

return FALSE;
}

#endif
/* EOF */