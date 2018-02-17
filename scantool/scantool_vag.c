/*
 * !!! INCOMPLETE !!!!
 *
 *	freediag - Vehicle Diagnostic Utility
 *
 *
 * Copyright (C) 2001 Richard Almeida & Ibex Ltd (rpa@ibex.co.uk)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *************************************************************************
 *
 *
 * Mostly ODBII Compliant Scan Tool (as defined in SAE J1978)
 *
 * CLI routines - vag subcommand
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>

#include "diag.h"
#include "diag_l1.h"
#include "diag_l2.h"
#include "diag_err.h"
#include "diag_os.h"

#include "diag_l7_vag.h"

#include "diag_vag.h"

#include "scantool.h"
#include "scantool_cli.h"

#include "diag_l2_vag.h"


static bool have_read_dtcs = false;

static int cmd_vw_connect(int argc, char **argv);
static int cmd_vw_disconnect(int argc, UNUSED(char **argv));
static int cmd_vw_id(int argc, UNUSED(char **argv));
static int cmd_vw_dtc(int argc, UNUSED(char **argv));
static int cmd_vw_cleardtc(int argc, UNUSED(char **argv));
static int cmd_vw_readblock(int argc, char **argv);


static int cmd_vag_help(int argc, char **argv);
const struct cmd_tbl_entry vag_cmd_table[] =
{
	{ "help", "help [command]", "Gives help for a command",
		cmd_vag_help, 0, NULL},
	{ "?", "? [command]", "Gives help for a command",
		cmd_vag_help, 0, NULL},

	{ "connect", "connect <ecu_name>", "Connect to ECU. Use 'vw connect ?' to show ECU names.",
		cmd_vw_connect, 0, NULL},
	{ "disconnect", "disconnect", "Disconnect from ECU",
		cmd_vw_disconnect, 0, NULL},
	{ "id", "id", "Display ECU identification",
		cmd_vw_id, 0, NULL},
	{ "dtc", "dtc", "Retrieve DTCs",
		cmd_vw_dtc, 0, NULL},
	{ "cleardtc", "cleardtc", "Clear DTCs from ECU",
		cmd_vw_cleardtc, 0, NULL},

	{ "readblock", "readblock <block no> [block no2] [block no3] [cont]", "Display live data, once or continuously", //TODO OGARNAC TEKST
		cmd_vw_readblock, 0, NULL},

	{ "up", "up", "Return to previous menu level",
		cmd_up, 0, NULL},
	{ "quit","quit", "Exit program",
		cmd_exit, FLAG_HIDDEN, NULL},
	{ "exit", "exit", "Exit program",
		cmd_exit, 0, NULL},

	{ NULL, NULL, NULL, NULL, 0, NULL}
};

/*
 * Table of english descriptions of the VW ECU addresses
 */
struct vw_id_info {
	uint8_t addr;
	char *name;
	char *desc;
	char *dtc_prefix;
};

static struct vw_id_info vw_id_list[] = {
	{0x01, "abs", "antilock brakes", "ABS"},
#if 0
	/*
	 * Address 0x10 communicates by KWP71 protocol, so we currently
	 * can't talk to it.
	 */
	{0x10, "m43", "Motronic M4.3 engine management (DLC pin 3)", "EFI"}, /* 12700bps */
	{0x10, "m44old", "Motronic M4.4 engine management (old protocol)", "EFI"}, /* 9600bps */
#endif
	{0x01, "eng", "ENGINE" ,"ENG"},
	{0x17, "clu", "Instrument clusters (0x17)", "CLU"},

	{0, NULL, NULL, NULL}
};

struct dtc_table_entry {
	uint8_t ecu_addr;
	uint8_t raw_value;
	uint16_t dtc_suffix;
	char *desc;
};


static struct dtc_table_entry dtc_table[] = {
	{0x6e, 0x13, 332, "Torque converter lock-up solenoid open circuit"},
	{0x7a, 0x54, 445, "Pulsed secondary air injection system pump signal"},
	{0, 0, 0, NULL}
};



/*
 * Check whether the number of arguments to a command is between the specified
 * minimum and maximum. If not, print a message and return false.
 */
static bool
valid_arg_count(int min, int argc, int max)
{
	if(argc < min) {
                printf("Too few arguments\n");
                return false;
	}

	if(argc > max) {
		printf("Too many arguments\n");
		return false;
	}

	return true;
}




/*
 * Look up an ECU by name.
 */
static struct vw_id_info *
ecu_info_by_name(const char *name)
{
	struct vw_id_info *ecu;

	for (ecu = vw_id_list; ecu->name != NULL; ecu++) {
		if (strcasecmp(name, ecu->name) == 0)
			return ecu;
	}

	return NULL;
}

/*
 * Get an ECU's address by name.
 */
static int
ecu_addr_by_name(const char *name)
{
	struct vw_id_info *ecu;
	unsigned long int i;
	char *p;

	if (isdigit(name[0])) {
		i = strtoul(name, &p, 0);
		if (*p != '\0')
			return -1;
		if(i > 0x7f)
			return -1;
		return i;
	}

	ecu = ecu_info_by_name(name);
	if (ecu == NULL) {
		return -1;
	} else {
		return ecu->addr;
	}
}

/*
 * Parse readblock and print data.
 */
static int
parse_and_print_block(char * block_raw, int block_no, int ecu_no)
{
	printf("BLOCK %03d:",block_no);

	//TODO DODAC TIMESTAMP
	// TODO SPRAWDZIC JAK TO DZIALA NA ZYWYM ORGANIZMIE
	// ogarnac rzutowanie, tam gdzie trzeba

	for(int j=0; j<12; j=j+3)	
	{
		uint8_t a= block_raw[j+1];
		uint8_t b= block_raw[j+2];

		switch( block_raw[j] )
		{
		case 0:
			printf("\t NULL");
			break;
		case 1:
			printf("\t %f RPM",0.2*a*b);
			break;
		case 2:
			printf("\t %f \%",a*0.002*b);
			break;	
		case 3:
			printf("\t %f Deg",0.002*a*b);
			break;	
		case 4:
			//printf("\t %f Deg",0.002*a*b);//TODO
			break;	
		case 5:
			printf("\t %f Deg",a*(b-100)*0.1);
			break;	
		case 6:
			printf("\t %f V",0.001*a*b);
			break;	
		case 7:
			printf("\t %f ",0.1*a*b);
			break;	
		case 8:
			printf("\t %f km/h",0.01*a*b);
			break;	
		case 9:
			//printf("\t %f Deg",0.002*a*b);//TODO
			break;	
		case 10:
			//printf("\t %f Deg",0.002*a*b);//TODO
			break;	
		case 11:
			printf("\t %f ",0.0001*a*(b-128)+1);
			break;	
		case 12:
			printf("\t %f Ohm",0.001*a*b);
			break;	
		case 13:
			printf("\t %f mm",(b-127)*0.001*a);
			break;	
		case 14:
			printf("\t %f bar",0.005*a*b);
			break;	

		default:
			printf("\t RAW 0x%02X 0x%02X 0x%02X",block_raw[j],a,b);
			break;
		}



	}
printf("\n");
return 0;
}

/*
 * Parse readblock argument.
 */
static int
parse_block_arg(const char *arg)
{
	
	unsigned long int i;
	char *p;

	if (!strcmp(arg,"cont"))
		return 0x100; //TODO zastanowic sie czy nie wprowadzic flagi statusu jakiejs


	if (isdigit(arg[0])) {
		i = strtoul(arg, &p, 0);
		if (*p != '\0')
			return -1;
		if(i > 0xFF || i==0)
			return -1;
		return i;
	}

	return -1;
}

/*
 * Get an ECU's description by address.
 */
static char *
ecu_desc_by_addr(uint8_t addr)
{
	struct vw_id_info *ecu;
	static char buf[7];

	for (ecu = vw_id_list; ecu->name != NULL; ecu++) {
		if (addr == ecu->addr)
			return ecu->desc;
	}

	sprintf(buf, "ECU %02X", addr);
	return buf;
}

/*
 * Get the description of the currently connected ECU.
 */
static char *
current_ecu_desc(void)
{
	int addr;

	if (global_state < STATE_CONNECTED)
		return "???";

	addr = global_l2_conn->diag_l2_destaddr;

	if((addr-128 < 0) || (addr-128 > 0x7f)) // TODO USTALIC CO Z TYM 128
		return "???";

	return ecu_desc_by_addr(addr-128); // TODO USTALIC CO Z TYM 128
}

/*
 * Indicates whether we're currently connected.
 */
static enum {
	NOT_CONNECTED, CONNECTED_VAG, CONNECTED_OTHER
} connection_status(void)
{
	if (global_state < STATE_CONNECTED) {
		return NOT_CONNECTED;
	} else if (global_l2_conn->l2proto->diag_l2_protocol == DIAG_L2_PROT_VAG) {
		return CONNECTED_VAG;
	} else {
		return CONNECTED_OTHER;
	}
}

/*
 * Check whether the connection status matches the required connection status
 * for this command. If not, print a message and return false.
 */
static bool
valid_connection_status(unsigned int want)
{
	if (connection_status() == want)
		return true;

	switch(connection_status()) {
	case NOT_CONNECTED:
		printf("Not connected.\n");
		return false;
	case CONNECTED_OTHER:
		if(want == NOT_CONNECTED) {
			printf("Already connected with non-VW protocol. Please use 'diag disconnect'.\n");
		} else {
			printf("Connected with non-VW protocol.\n");
		}
		return false;
	case CONNECTED_VAG:
		printf("Already connected to %s. Please disconnect first.\n", current_ecu_desc());
		return false;
	default:
		printf("Unexpected connection state!\n");
		return false;
	}
}


static int
cmd_vag_help(int argc, char **argv)
{
	return help_common(argc, argv, vag_cmd_table);
}

/*
 * Capitalize the first letter of the supplied string.
 * Returns a static buffer that will be reused on the next call.
 */
static char *
capitalize(const char *in)
{
	static char buf[80];

	strncpy(buf, in, sizeof(buf));
	buf[sizeof(buf)-1] = '\0';

	if(isalpha(buf[0]) && islower(buf[0]))
		buf[0] = toupper(buf[0]);
	return buf;
}


/*
 * Print a list of known ECUs. Not all ECUs in this list are necessarily
 * present in the vehicle.
 */
static void
print_ecu_list(void)
{
	struct vw_id_info *ecu;

	for (ecu = vw_id_list; ecu->name != NULL; ecu++) {
		printf(" %s\t%s\n", ecu->name, capitalize(ecu->desc));
	}
}

/*
 * Get the printable designation (EFI-xxx, AT-xxx, etc) for a DTC by its raw
 * byte value. Optionally, also get a description of the DTC.
 * Returns a static buffer that will be reused on the next call.
 */
static char *
dtc_printable_by_raw(uint8_t addr, uint8_t raw, char **desc)
{
	static char printable[7+1];
	static char *empty="";
	struct vw_id_info *ecu_entry;
	struct dtc_table_entry *dtc_entry;
	char *prefix;
	uint16_t suffix;

	prefix = "???";
	for (ecu_entry = vw_id_list; ecu_entry->name != NULL; ecu_entry++) {
		if (addr == ecu_entry->addr) {
			prefix = ecu_entry->dtc_prefix;
			break;
		}
	}

	for (dtc_entry = dtc_table; dtc_entry->dtc_suffix != 0; dtc_entry++) {
		if (dtc_entry->ecu_addr == addr && dtc_entry->raw_value == raw) {
			suffix = dtc_entry->dtc_suffix;
			if(desc != NULL)
				*desc = dtc_entry->desc;
			sprintf(printable, "%s-%03d", prefix, suffix);
			return printable;
		}
	}

	if (desc != NULL)
		*desc = empty;
	sprintf(printable, "%s-???", prefix);
	return printable;
}




/*
 * Display ECU identification.
 */
static int
cmd_vw_id(int argc, UNUSED(char **argv))
{
	struct diag_l2_vag * dl2v = global_l2_conn->diag_l2_proto_data;
	struct diag_msg *ecu_id=dl2v->ecu_id_telegram; 

	if (!valid_arg_count(1, argc, 1))
		return CMD_USAGE;

	if(!valid_connection_status(CONNECTED_VAG))
		return CMD_OK;

	


	for(int i=0; ecu_id; i++)
	{
	printf("ID%d: ", i);

	for(int j=0; j<ecu_id->len; j++) printf("%c",(ecu_id->data[j])&0x7F);

	printf("\n");
	ecu_id=ecu_id->next;
	}

	ecu_id=dl2v->ecu_id_telegram;
	for(int i=0; ecu_id; i++)
	{
	printf("ID%d: ", i);

	for(int j=0; j<ecu_id->len; j++) printf("0x%2X ",(ecu_id->data[j]));

	printf("\n");
	ecu_id=ecu_id->next;
	}

	return CMD_OK;
}

/*
 * Read and display one or more live data parameters.
 *
 * Takes a list of one-byte identifier values. If a value is prefixed with *,
 * it is treated as an address or address range to read from RAM instead of
 * a live data parameter identifier; in this way, a list of "read" and "peek"
 * operations can be done in a single command.
 *
 * The word "live" can be added at the end to continuously read and display
 * the requested addresses until interrupted. //TODO POPRAWIC OPIS
 */
static int
cmd_vw_readblock(int argc, char **argv)
{
	bool cont_flag=false;
	uint8_t readblocks[4]={0};

	if (!valid_arg_count(2, argc, 5)) //valid_arg_count(int min, int argc, int max)
		return CMD_USAGE;

	if(!valid_connection_status(CONNECTED_VAG))
		return CMD_OK;

	for(int i=1;i<argc;i++)
	{
		int temp=parse_block_arg(argv[i]);

		switch( temp )
		{
		case 0x100:
			cont_flag=true;
			break;
		
		case -1:
			return CMD_USAGE;
			break;
		
		default:
			readblocks[0]++;
			readblocks[readblocks[0]]=temp;
			break;
		}				


	}

	if (!readblocks[0]) 
		return CMD_USAGE;


for(int i=1;i<=readblocks[0];i++)
	{
		char block_temp[12]={0};

	diag_l7_vag_readblock(global_l2_conn, readblocks[i], block_temp);		
	parse_and_print_block(block_temp, 0,0);

	}




	return CMD_OK;
}

/*
 * Display list of stored DTCs.
 */
static int
cmd_vw_dtc(int argc, UNUSED(char **argv))
{
	struct vw_dtc_data dtc_list[16];

	//uint8_t buf[12];
	int rv;
	int i;
	char *code, *desc;

	if (!valid_arg_count(1, argc, 1))
		return CMD_USAGE;

	if(!valid_connection_status(CONNECTED_VAG))
		return CMD_OK;

	rv = diag_l7_vag_dtclist(global_l2_conn, 16, dtc_list);
	if (rv < 0) {
		printf("Couldn't retrieve DTCs.\n");
		return CMD_OK;
	}
	have_read_dtcs = true;

	if (rv == 0) {
		printf("No stored DTCs.\n");
		return CMD_OK;
	}

	printf("Stored DTCs:\n");
	for (i=0; i<rv; i++) {
		//code = dtc_printable_by_raw(global_l2_conn->diag_l2_destaddr, buf[i], &desc);
		//printf("%s (%02X) %s\n", code, buf[i], desc);
		printf("%05d (0x%04X) %03d (0x%02X)\n", dtc_list[i].value, dtc_list[i].value, dtc_list[i].status, dtc_list[i].status); //TODO STATUSY DOROBIC
	}

	return CMD_OK;
}

/*
 * Clear stored DTCs.
 */
static int
cmd_vw_cleardtc(int argc, UNUSED(char **argv))
{
	char *input;
	int rv;

	if (!valid_arg_count(1, argc, 1))
		return CMD_USAGE;

	if(!valid_connection_status(CONNECTED_VAG))
		return CMD_OK;

	input = basic_get_input("Are you sure you wish to clear the Diagnostic Trouble Codes (y/n) ? ", stdin);
	if (!input)
		return CMD_OK;

	if ((strcasecmp(input, "yes") != 0) && (strcasecmp(input, "y")!=0)) {
		printf("Not done\n");
		goto done;
	}

	if (!have_read_dtcs) {
		free(input);
		input = basic_get_input("You haven't read the DTCs yet. Are you sure you wish to clear them (y/n) ? ", stdin);
		if (!input)
			return CMD_OK;
		if ((strcasecmp(input, "yes") != 0) && (strcasecmp(input, "y")!=0)) {
			printf("Not done\n");
			goto done;
		}
	}

	rv = diag_l7_vag_cleardtc(global_l2_conn);
	if (rv == 0) {
		printf("No DTCs to clear!\n");
	} else if (rv == 1) {
		printf("Done\n");
	} else {
		printf("Failed\n");
	}

done:
	free(input);
	return CMD_OK;
}


/*
 * Connect to an ECU by name or address.
 */
static int
cmd_vw_connect(int argc, char **argv)
{
	int addr;
	int rv;
	struct diag_l0_device *dl0d;


	if (!valid_arg_count(2, argc, 2))
                return CMD_USAGE;

	if (strcmp(argv[1], "?") == 0) {
		printf("Known ECUs are:\n");
		print_ecu_list();
		printf("Can also specify target by numeric address.\n");
		return CMD_USAGE;
	}

	if(!valid_connection_status(NOT_CONNECTED))
		return CMD_OK;

	addr = ecu_addr_by_name(argv[1]) +128; // TODO USTALIC CO Z TYM 128
	if (addr < 0) {
		printf("Unknown ECU '%s'\n", argv[1]);
		return CMD_OK;
	}

	global_cfg.speed = 10400; //TODO OGARNAC OPCJE INICJALIZACJI
	global_cfg.src = 0xF1;
	global_cfg.tgt = addr;
	global_cfg.L1proto = DIAG_L1_ISO9141;
	global_cfg.L2proto = DIAG_L2_PROT_VAG;
	global_cfg.initmode = DIAG_L2_TYPE_SLOWINIT;

	dl0d = global_dl0d;

	if (dl0d == NULL) {
		printf("No global L0. Please select + configure L0 first\n");
		return diag_iseterr(DIAG_ERR_GENERAL);
	}

	rv = diag_init();
	if (rv != 0) {
		fprintf(stderr, "diag_init failed\n");
		diag_end();
		return diag_iseterr(rv);
	}

	rv = diag_l2_open(dl0d, global_cfg.L1proto);
	if (rv) {
		fprintf(stderr, "cmd_vw_connect: diag_l2_open failed\n");
		return diag_iseterr(rv);
	}

	global_l2_conn = diag_l2_StartCommunications(dl0d, global_cfg.L2proto,
		global_cfg.initmode & DIAG_L2_TYPE_INITMASK, global_cfg.speed,
		global_cfg.tgt, global_cfg.src);
	if (global_l2_conn == NULL) {
		rv = diag_geterr();
		diag_l2_close(dl0d);
		return diag_iseterr(rv);
	}

	global_state = STATE_CONNECTED;

	printf("Connected to %s.\n", ecu_desc_by_addr(addr-128));// TODO USTALIC CO Z TYM 128
	have_read_dtcs = false;

	cmd_vw_id(1, NULL);

	return CMD_OK;
}

/*
 * Close the current connection.
 */
static int
cmd_vw_disconnect(int argc, UNUSED(char **argv))
{
	char *desc;

	if (!valid_arg_count(1, argc, 1))
		return CMD_USAGE;

	if(!valid_connection_status(CONNECTED_VAG))
		return CMD_OK;

	desc = current_ecu_desc();

	diag_l2_StopCommunications(global_l2_conn);
	diag_l2_close(global_dl0d);

	global_l2_conn = NULL;
	global_state = STATE_IDLE;

	printf("Disconnected from %s.\n", desc);
	have_read_dtcs = false;
	return CMD_OK;
}








