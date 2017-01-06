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
 * oc_get_swr()			: get switch & knob settings from console
 * oc_mmu()			: toggle mmu mapping mode (16/18/22 bit)
 * oc_poll()			: pool input channel for data ready
 * oc_port1()			: toggle bit on/off in port1 flagbyte
 * oc_port2()			: toggle bit on/off in port2 flagbyte
 * oc_send_cmd()		: send single byte command
 * oc_read_line_p()		: get command (keyboard or console)
 * oc_reset()			: device reset
 * oc_ringprot()		: toggle ring protection bits (USK)
 * oc_show()			: show status of the device (link)
 * oc_svc()			: service routine
 * oc_send_ack()		: acknoledge a state on the console processor
 * oc_help()			: Generic help
 * oc_help_attach()		: Help for attach command
 *
 * More information can be found in the doc/opcon_doc.txt file
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <signal.h>
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
extern UNIT cpu_unit;			/* configured memory size */
extern int32 sim_do_echo;

/* Defines */
#define OC_INTERVAL	     1000	/* # of nanoseconds */

/* Debug levels for the OC device */
#define OCDEB_CON	      001	/* console input */
#define OCDEB_HLT	      002	/* halt switch check */
#define OCDEB_STS	      004	/* status leds update */
#define OCDEB_SWR	      010	/* switch register queries */
#define OCDEB_SVC	      020	/* service calls  */
#define OCDEB_TRC	      040	/* trace calls */
#define OCDEB_UPD	      100	/* address & data leds update */

/* Global declarations for the OC device */
oc_st	*ocp;				/* OC device control block */

/* Debug flags & keywords for the OC device */
DEBTAB oc_debug[] = {
    { "CON", OCDEB_CON },		/* used in  oc_get_console */
    { "STS", OCDEB_STS },		/* used in oc_send_cmd (status) */
    { "SWR", OCDEB_SWR },		/* used in oc_get_swr */
    { "SVC", OCDEB_SVC },		/* used in oc_svc */
    { "TRC", OCDEB_TRC },		/* used in all major entry points */
    { "UPD", OCDEB_UPD },		/* used in oc_send_cmd (update)*/
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
uint8 oc_active = FALSE;
uint32 oc_console_pid = 0;

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
uint32 H, K, i;
t_stat r;
SERHANDLE p;
key_t oc_key = 201604;
char con_av[2][20] = {
	  { "0x00000000" },
	  { "0x00000000" }
	};

sim_debug (OCDEB_TRC, &oc_dev, "oc_attach : called\n");

if (cpu_model != MOD_1105 && cpu_model != MOD_1120 && cpu_model != MOD_1140 &&
    cpu_model != MOD_1145 && cpu_model != MOD_1170) {
  printf ("OC    : No support for the current cpu model.\n");
  return SCPE_OK;
  } 

if (cptr == NULL)
    return SCPE_ARG;

if ((tptr = strchr (cptr, '=')) == NULL)
    return SCPE_ARG;
*tptr++; 

if ((p = sim_open_serial (tptr, NULL, &r)) == (SERHANDLE)-1) {	/* port usable? */
    sim_close_serial (p);
    printf("OC    : Cannot open '%s' for usability check (errno = %d).\n", tptr, errno);
    return -1;
    }
else
  sim_close_serial(p);		/* close it, else CPB has a problem */

if ((i = shmget(oc_key, sizeof(oc_st), (IPC_CREAT | 0666))) < 0 ||
    (ocp = (oc_st *)shmat(i, NULL, 0)) == (oc_st *)-1)  {
  printf("OC    : Cannot create or attach shm segment (errno = %d).\n", errno);
  return -1;
  }

memset(ocp, 0, sizeof(oc_st));
switch (cpu_model) {
    case MOD_1105 : ocp->cpu_model = 1; break;
    case MOD_1120 : ocp->cpu_model = 2; break;
    case MOD_1140 : ocp->cpu_model = 3; break;
    case MOD_1145 : ocp->cpu_model = 4; break;
    case MOD_1170 : ocp->cpu_model = 5; break;
    } 
strcpy(ocp->line, tptr);
ocp->first_exam = TRUE;
ocp->first_dep = TRUE;
ocp->A[0] = 0xFF;

sprintf(con_av[0], "0X%08X", ocp);
if ((oc_console_pid = fork()) == -1) {
  printf("OC    : Cannot fork for console processor.\n");
  if (ocp != 0) shmdt(ocp);
  return -1;
  }

if (oc_console_pid == 0) {	/* I'm the child */
  alarm(0);
  execl("console", "cons1170", con_av);
  printf("OC    : Exec of console processor task failed.\n");
  _exit(1);
  }

/* wait for address [0] to become cleared */
for (i = 0; ocp->A[0] != 0 && i < 5; i++)
  sleep(1);

if (ocp->A[0] != 0) {
  kill(oc_console_pid, SIGHUP);
  if (ocp != 0) shmdt(ocp);
  oc_console_pid = 0;
  printf("OC    : Console processor failed to start.\n");
  return -1;
  }

/* now we are in business */
oc_active = TRUE;					/* mark as available */

oc_get_swr ();				/* request console key state */

switch (cpu_model) {			/* detect power & halt key position */
    case MOD_1105 : K = ocp->S[INP2] & SW_PL_1105;
		    H = ocp->S[INP2] & SW_HE_1105; break;
    case MOD_1120 : K = ocp->S[INP2] & SW_PL_1120;
		    H = ocp->S[INP2] & SW_HE_1120; break;
    case MOD_1140 : K = ocp->S[INP2] & SW_PL_1140;
		    H = ocp->S[INP2] & SW_HE_1140; break;
    case MOD_1145 : K = ocp->S[INP3] & SW_PL_1145; 
		    H = ocp->S[INP5] & SW_HE_1145; break;
    case MOD_1170 : K = ocp->S[INP5] & SW_PL_1170;
		    H = ocp->S[INP5] & SW_HE_1170; break;
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
	ocp->HALT = 2;
	if (!sim_quiet)
	    printf ("HALT\n");
        }
    }
else
    if (!sim_quiet)
	printf ("LOCK\n");

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

if (!oc_active)					/* return if already closed */
    return SCPE_OK;

sim_cancel (&oc_unit);				/* dequeue service routine */

if (oc_console_pid != 0) {
  kill(SIGHUP, oc_console_pid);
  oc_console_pid = 0;
  }

if (ocp != 0) shmdt(ocp);

oc_active = FALSE;				/* clear receiver flag */

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
 * Note	    : This is the service routine.
 *
 * Returns  : SCPE_OK
 */
t_stat oc_svc (UNIT *uptr)
{
if (!oc_active)			/* console link configured / open? */
    return FALSE;
    
sim_debug (OCDEB_TRC, &oc_dev, "oc_svc : called\n");
oc_port1(FSTS_1170_INDDATA, ocp->ind_addr);
sim_activate_after (uptr, 2 * OC_INTERVAL);		/* reschedule */
return SCPE_OK;
}

/*
 * Function : oc_show()
 * Note	    : Show the status of the link
 * Returns  : SCPE_OK
 */
t_stat oc_show (FILE *st, UNIT *uptr, int32 val, void *desc)
{
if (oc_active)
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
" OC11 Remote Operator Console processor subsystem\n"
"\n"
" The OC11 is a pseudo driver and is an interface to the core-IO console\n"
" processor which allows an original PDP-11 operator console to control the\n"
" behaviour of SIMH.\n"
" Actual address, data & status information is checked for each simulated\n"
" SIMH instruction. A shared memory segment is used for the exchange.\n"
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
" The OC driver creates a small shared memory segment for exchange with a 2nd process"
" which does the actual communication with the the console processor hardware."
"\n"
" The ATTACH command specifies which serial port to be used (is passed to the 2nd process).\n"
" A serial port may be specified as an operating system specific device name\n"
" or useing simh generic serial name. Simh generic names are of the form\n"
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
" This will detach the shared segment and destory the 2nd process.\n"
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
uint16 D;
uint32 A;

sim_debug (OCDEB_TRC, &oc_dev, "oc_get_console : called\n");

if (!oc_active)			/* console link configured / open? */
    return FALSE;

if (ocp->IN == 0)		/* no cmd byte from CPB */
  return FALSE;

sim_debug (OCDEB_CON, &oc_dev, "oc_get_console : byte = 0x%02X (%c)\n", 
	ocp->IN, ocp->IN);

switch (ocp->IN) {
    case 'H' :					/* HALT/ENABLE->HALT */
	ocp->HALT = 2;
	strcpy (cptr, ";halt key down\n");
	break;
    case 'E' :					/* HALT/ENABLE->ENABLE */
	ocp->HALT = 1;
	strcpy (cptr, ";halt key up\n");
//	oc_send_ack (ACK_CONT);
	oc_send_cmd ('a');	/* clear all toggles */
	break;
    case 'c' :					/* CONTINUE */
	oc_send_ack (ACK_CONT);
	if (ocp->HALT == 2)
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
	if (ocp->first_dep == FALSE) {	/* 1st dep cmd? */
	    if ((ocp->act_addr >= 0x3FFC0) && 
		(ocp->act_addr <= 0x3FFC7))
		ocp->act_addr += 1;	/* in CPU register space +1 */
	    else {
		ocp->act_addr += 2;	/* rest of address space +2 */
		if (ocp->act_addr > 0x3FFFFE) /* 22 bit overflow? */
		    ocp->act_addr = 0;
		if ((ocp->act_addr & 1) != 0)/* make odd adr even */
		    ocp->act_addr &= 0x3FFFFE;
		}
	    }
	if (ocp->inv_addr) {	/* above mem range? */
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
	    if ((((ocp->act_addr & 0x0003FFFF) >= 0x3FEA00) &&  /*17765000 */
	         ((ocp->act_addr & 0x0003FFFF) <  0x3FEC00)) ||
		(((ocp->act_addr & 0x0003FFFF) >= 0xEA00) &&	  /*  165000 */
	         ((ocp->act_addr & 0x0003FFFF) <  0xEC00)) ||
		(((ocp->act_addr & 0x0003FFFF) >= 0x3FF600) &&  /*17775000 */
	         ((ocp->act_addr & 0x0003FFFF) <  0x3FF800)) ||
		(((ocp->act_addr & 0x0003FFFF) >= 0xF600) &&	  /*  175000 */
	         ((ocp->act_addr & 0x0003FFFF) <  0xF800))) {
		strcpy (cptr, ";no deposit in boot rom range\n");
		}
	    else {
		D = oc_extract_data ();		/* get 'data' data */
		ocp->first_exam = TRUE;
		ocp->first_dep = FALSE;
		ocp->D[0] = D;
		oc_send_cmd('B'); 	/* send A & D */
		sprintf (cptr, "deposit %o %o\n", ocp->act_addr, D);
		}
	    }
	oc_send_ack (ACK_DEPO);		/* ack */
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
	ocp->first_dep = TRUE;
	ocp->first_exam = TRUE;
	oc_send_cmd('A');	/* send address to display */
	sprintf (cptr, ";load address %08o\n", ocp->act_addr);
	oc_send_ack (ACK_LOAD) ;
	break;
    case 's' :					/* START */
	if (ocp->HALT == 2) {
	    strcpy (cptr, "reset all\n");	/* RESET when HALT is down */
	    if (cpu_model == MOD_1170)
		oc_port1 (FSTS_1170_ADRSERR, 0);
	    }
	else
	    sprintf (cptr, "run %o\n", ocp->act_addr);
//	oc_send_ack (ACK_START);
	oc_clear_halt ();
	break;
    case 'x' :					/* EXAMINE */
	if (ocp->first_exam == FALSE) { /* not 1st EXAM: auto-incr. */
	    if ((ocp->act_addr >= 0x3FFC0) && 
		(ocp->act_addr <= 0x3FFC7))
	        ocp->act_addr += 1;	/* in CPU register space +1 */
	    else {
	        ocp->act_addr += 2;	/* rest of address space +2 */
	        if (ocp->act_addr > 0x3FFFFE) /* 22-bit overflow? */
		    ocp->act_addr = 0;
	        if ((ocp->act_addr & 1) != 0)	/* make odd adr even */
		    ocp->act_addr &= 0x3FFFFE;
	        }
	    }
	if (ocp->inv_addr) {
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
	    ocp->first_exam = FALSE;
	    ocp->first_dep = TRUE;
	    oc_send_cmd('A');
	    sprintf (cptr, "examine %o\n", ocp->act_addr); 
	    }
	oc_send_ack (ACK_EXAM);
	break;
    default :				/* stray byte? just ignore it */
	return FALSE;
	break;
    }
ocp->IN = 0;				/* allow for next cmd */
oc_send_cmd('F');			/* update console status leds */
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

A = ocp->S[SWR_16_22_PORT] * 65536 +
    ocp->S[SWR_08_15_PORT] * 256   +
    ocp->S[SWR_00_07_PORT];

ocp->inv_addr = FALSE;
switch (cpu_model) {		/* I/O page is not out of address range */
    case MOD_1105 :
    case MOD_1120 : A &= 0x0000FFFF; 		/* max 64Kb */
		    if (A >= MEMSIZE && !(A > 0xDFFF && A < 0xFFFF))
			ocp->inv_addr = TRUE;	
		    break;
    case MOD_1140 :
    case MOD_1145 : A &= 0x0003FFFF; 		/* max 256Kb */
		    if (A >= MEMSIZE && !(A > 0x3DFFF && A < 0x3FFFF) )
			ocp->inv_addr = TRUE;	
		    break;
    case MOD_1170 : A &= 0x003FFFFF;       	/* max 4Mb */
		    if (A >= MEMSIZE && !(A > 0x3FDFFF && A < 0x3FFFFF))
			ocp->inv_addr = TRUE;	
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
return (ocp->S[SWR_08_15_PORT] * 256 + ocp->S[SWR_00_07_PORT]);
}

/*
 * Function : oc_port1()
 * Note     : Toggle a single bit for port1
 * Returns  : Nothing
 */
void oc_port1 (uint8 flag, t_bool action)
{
if (!action)
    ocp->PORT1 = (ocp->PORT1 & (~flag));
else
    ocp->PORT1 = (ocp->PORT1 | flag);
}

/*
 * Function : oc_port2()
 * Note     : Toggle a single bit for port1
 * Returns  : Nothing
 */
void oc_port2 (uint8 flag, t_bool action)
{
if (!action)
    ocp->PORT2 = (ocp->PORT2 & (~flag));
else
    ocp->PORT2 = (ocp->PORT2 | flag);
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
if (ocp->MMR0 & MMR0_MME) {
    map = 18;
    if (ocp->MMR3 & MMR3_M22E)
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
    case MOD_1170 : status = ocp->PORT1 | 0x03;
		    if (value == MD_KER) status &= 0xFC;
		    if (value == MD_SUP) status &= 0xFD;
		    if (value == MD_USR) status &= 0xFF;
		    ocp->PORT1 = status;
		    break;
    }
}

/*
 * Function : oc_send_cmd()
 * Note     : Send a single command to the console task.
 * Returns  : Nothing
 */
void oc_send_cmd(uint8 cmd)
{
switch(cmd) {
  case 'A' : sim_debug(OCDEB_TRC, &oc_dev, "oc_send_cmd (A): address %06X\n",
		ocp->act_addr);
	     break;
  case 'B' : sim_debug(OCDEB_UPD, &oc_dev,
		"oc_send_cmd (A&D) : A:0x%06X D:0x%04X\n", 
		ocp->act_addr, ocp->D[0]);
	     break;
  case 'F' : sim_debug(OCDEB_STS, &oc_dev, 
		"oc_send_cmd (status) : byte1 0x%X, byte2 : 0x%X\n",
		ocp->PORT1, ocp->PORT2);
	     break;
  case 'a' : sim_debug (OCDEB_TRC, &oc_dev, "oc_send_cmd (clr all toggles)\n");
	     break;
  default  :
	     break;
  }
ocp->OUT = cmd;
while(ocp->OUT != 0) usleep(500);
}

/*
 * Function : oc_get_swr()
 * Note     : Send the Query command to the operator console 
 *	      Then read the amount of bytes representing the status of all
 *	      the switches on the operator console, stored in the array
 *  	      "ocp->S[]".
 *  Returns : 0 or -1
 */
int oc_get_swr (void)
{
char brk = 0;
uint8 c = 'Q';
uint32 K;
int x;

sim_debug (OCDEB_TRC, &oc_dev, "oc_get_swr : called\n");

ocp->OUT = 'Q';
while(ocp->OUT != 0) usleep(500);

sim_debug (OCDEB_SWR, &oc_dev,
	"oc_get_swr : swreg bytes = 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
	ocp->S[0], ocp->S[1], ocp->S[2], ocp->S[3], ocp->S[4]);

return 0;
}

/*
 * Function : oc_send_ack()
 * Note     : Send the clear-toggle command with data
 * Returns  : Nothing
 */
void oc_send_ack (uint8 mask)
{
char *msg;

sim_debug (OCDEB_TRC, &oc_dev, "oc_send_ack : called, mask = %d\n", mask);

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
ocp->ACK[0] = 'c';
switch (cpu_model) {
    case MOD_1105 : ocp->ACK[1] = (uint8)(INP3 + 0x30); break;
    case MOD_1120 : ocp->ACK[1] = (uint8)(INP3 + 0x30); break;
    case MOD_1140 : ocp->ACK[1] = (uint8)(INP3 + 0x30); break;
    case MOD_1145 : ocp->ACK[1] = (uint8)(INP3 + 0x30); break;
    case MOD_1170 : ocp->ACK[1] = (uint8)(INP3 + 0x30); break;
    }
ocp->ACK[2] = mask;
ocp->OUT = 'o';
while(ocp->OUT != 0) usleep(1000);
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
    if (oc_active)
      oc_master (1);
    if (oc_get_console (cptr) == TRUE) { /* poll console for data */
        printf ("%s", cptr);
        break;
        }

    if (oc_poll (0, 10000) == 1 &&		/* wait for data on keyboard */
        sim_read_serial (0, &key_entry, 1, &brk) > 0) {
        if (key_entry == '\b' && *tptr > *cptr) {	/* backspace */
            *tptr--;
            sim_write_serial ((SERHANDLE)1, &key_entry, 1);	
            sim_write_serial ((SERHANDLE)1, " ", 1);
            sim_write_serial ((SERHANDLE)1, &key_entry, 1);
            }
        else {					/* regular character */
            *tptr++ = key_entry;		/* store the character */
            sim_write_serial ((SERHANDLE)1, &key_entry, 1);/* echo the entry to crt */
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

if (oc_active) {
  oc_master (0);
  if (ocp->HALT == 1)		/* no stray mode flag */
     stop_cpu = 1;
  }

return cptr;			/* points to begin of cmd or 0*/
}

/*
 * Function : oc_halt_status()
 * Note     : Check the value of the ocp->HALT variable
 * Returns  : 0 - if halt mode = 0 or 1
 *          : 1 - if halt mode = 2
 */
int oc_halt_status (void)
{
if (oc_active)
  if (ocp->HALT == 2) return(1);

return(0);
}

/*
 * Function : oc_clear_halt()
 * Note     : Clear the halt bit in the swr array & clear all toggles
 * Returns  : Nothing
 */
void oc_clear_halt (void)
{
switch (cpu_model) {
    case MOD_1105 : ocp->S[INP2] = ocp->S[INP2] & (~SW_HE_1105);	break;
    case MOD_1120 : ocp->S[INP3] = ocp->S[INP3] & (~SW_HE_1120);	break;
    case MOD_1140 : ocp->S[INP3] = ocp->S[INP3] & (~SW_HE_1140);	break;
    case MOD_1145 : ocp->S[INP4] = ocp->S[INP4] & (~SW_HE_1145);	break;
    case MOD_1170 : ocp->S[INP4] = ocp->S[INP4] & (~SW_HE_1170);	break;
    }
ocp->HALT = 0;
oc_send_cmd('a');	/* clear all toggles */
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
