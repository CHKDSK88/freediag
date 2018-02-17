/*
 *      freediag - Vehicle Diagnostic Utility
 *
 *
 * Copyright (C) 2017 Adam Goldman
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
 * Diag
 *
 * vag protocol application layer
 *
 * This protocol is used by the engine and chassis ECUs for extended
 * diagnostics on the 1996-1998 vag 850, S40, C70, S70, V70, XC70, V90 and
 * possibly other models. On the aforementioned models, it is used over
 * KWP6227 (keyword D3 B0) transport. It seems that the same protocol is also
 * used over CAN bus on more recent models.
 *
 * Information on this command set is available at:
 *   http://jonesrh.info/vag850/vag_850_obdii_faq.rtf
 * Thanks to Richard H. Jones for sharing this information.
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "diag.h"
#include "diag_os.h"
#include "diag_err.h"
#include "diag_l1.h"
#include "diag_l2.h"
#include "diag_l7_vag.h"

/*
 * Service Identifier hex values are in the manufacturer defined range.
 * The service names used here are based on KWP2000. Original service names
 * are unknown. Request and response message formats for these services are
 * NOT according to KWP2000.
 */
enum {
	stopDiagnosticSession = 0xA0,
	testerPresent = 0xA1,
	readDataByLocalIdentifier = 0xA5,
	readDataByLongLocalIdentifier = 0xA6, /* CAN bus only? */
	readMemoryByAddress = 0xA7,
	readFreezeFrameByDTC = 0xAD,
	readDiagnosticTroubleCodes = 0xAE,
	clearDiagnosticInformation = 0xAF,
	readNVByLocalIdentifier = 0xB9
} service_id;


/*
 * Verify communication with the ECU.
 */
int
diag_l7_vag_ping(struct diag_l2_conn *d_l2_conn)
{
	uint8_t req[] = { testerPresent };
	int errval = 0;
	struct diag_msg msg = {0};
	struct diag_msg *resp = NULL;

	msg.data = req;
	msg.len = sizeof(req);

	resp = diag_l2_request(d_l2_conn, &msg, &errval);
	if (resp == NULL)
		return errval;

	/*if (success_p(&msg, resp)) {
		diag_freemsg(resp);
		return 0;
	} else */{
		diag_freemsg(resp);
		return DIAG_ERR_ECUSAIDNO;
	}
}

/* The request message for reading memory */
static int
read_MEMORY_req(uint8_t **msgout, unsigned int *msglen, uint16_t addr, uint8_t count)
{
	static uint8_t req[] = { readMemoryByAddress, 0, 99, 99, 1, 99 };

	req[2] = (addr>>8)&0xff;
	req[3] = addr&0xff;
	req[5] = count;
	*msgout = req;
	*msglen = sizeof(req);
	return 0;
}

/* The request message for reading live data by 1-byte identifier */
static int
read_LIVEDATA_req(uint8_t **msgout, unsigned int *msglen, uint16_t addr, UNUSED(uint8_t count))
{
	static uint8_t req[] = { readDataByLocalIdentifier, 99, 1 };

	req[1] = addr&0xff;
	if (addr > 0xff) {
		fprintf(stderr, FLFMT "read_LIVEDATA_req invalid address %x\n", FL, addr);
		return DIAG_ERR_GENERAL;
	}

	*msgout = req;
	*msglen = sizeof(req);
	return 0;
}

/* The request message for reading live data by 2-byte ident (CAN bus only?) */
static int
read_LIVEDATA2_req(uint8_t **msgout, unsigned int *msglen, uint16_t addr, UNUSED(uint8_t count))
{
	static uint8_t req[] = { readDataByLongLocalIdentifier, 99, 99, 1 };
	req[1] = (addr>>8)&0xff;
	req[2] = addr&0xff;
	*msgout = req;
	*msglen = sizeof(req);
	return 0;
}

/* The request message for reading non-volatile data */
static int
read_NV_req(uint8_t **msgout, unsigned int *msglen, uint16_t addr, UNUSED(uint8_t count))
{
	static uint8_t req[] = { readNVByLocalIdentifier, 99 };

	req[1] = addr&0xff;
	if (addr > 0xff) {
		fprintf(stderr, FLFMT "read_NV_req invalid address %x\n", FL, addr);
		return DIAG_ERR_GENERAL;
	}

	*msgout = req;
	*msglen = sizeof(req);
	return 0;
}

/* The request message for reading freeze frames */
static int
read_FREEZE_req(uint8_t **msgout, unsigned int *msglen, uint16_t addr, UNUSED(uint8_t count))
{
	static uint8_t req[] = { readFreezeFrameByDTC, 99, 0 };

	req[1] = addr&0xff;
	if (addr > 0xff) {
		fprintf(stderr, FLFMT "read_FREEZE_req invalid address %x\n", FL, addr);
		return DIAG_ERR_GENERAL;
	}

	*msgout = req;
	*msglen = sizeof(req);
	return 0;
}

/*
 * Read memory, live data or non-volatile data.
 *
 * Return value is actual byte count received, or negative on failure.
 *
 * For memory reads, a successful read always copies the exact number of bytes
 * requested into the output buffer.
 *
 * For live data, non-volatile data and freeze frame reads, copies up to the
 * number of bytes requested. Returns the actual byte count received, which may
 * be more or less than the number of bytes requested.
 */
int
diag_l7_vag_read(struct diag_l2_conn *d_l2_conn, enum namespace ns, uint16_t addr, int buflen, uint8_t *out)
{
	struct diag_msg req = {0};
	struct diag_msg *resp = NULL;
	int datalen;
	int rv;

	switch (ns) {
	case NS_MEMORY:
		rv = read_MEMORY_req(&req.data, &req.len, addr, buflen);
		break;
	case NS_LIVEDATA:
		rv = read_LIVEDATA_req(&req.data, &req.len, addr, buflen);
		break;
	case NS_LIVEDATA2:
		rv = read_LIVEDATA2_req(&req.data, &req.len, addr, buflen);
		break;
	case NS_NV:
		rv = read_NV_req(&req.data, &req.len, addr, buflen);
		break;
	case NS_FREEZE:
		rv = read_FREEZE_req(&req.data, &req.len, addr, buflen);
		break;
	default:
		fprintf(stderr, FLFMT "diag_l7_vag_read invalid namespace %d\n", FL, ns);
		return DIAG_ERR_GENERAL;
	}

	if (rv != 0)
		return rv;

	resp = diag_l2_request(d_l2_conn, &req, &rv);
	if (resp == NULL)
		return rv;

	if (resp->len<2 /*|| !success_p(&req, resp)*/) {
		diag_freemsg(resp);
		return DIAG_ERR_ECUSAIDNO;
	}

	if (ns==NS_MEMORY) {
		if (resp->len!=(unsigned int)buflen+4 || memcmp(req.data+1, resp->data+1, 3)!=0) {
			diag_freemsg(resp);
			return DIAG_ERR_ECUSAIDNO;
		}
		memcpy(out, resp->data+4, buflen);
		diag_freemsg(resp);
		return buflen;
	}

	datalen = resp->len - 2;
	if (datalen > 0)
		memcpy(out, resp->data+2, (datalen>buflen)?buflen:datalen);
	diag_freemsg(resp);
	return datalen;
}

/*
 * Retrieve list of stored DTCs.
 */
int
//diag_l7_vag_dtclist(struct diag_l2_conn *d_l2_conn, int buflen, uint8_t *out)
diag_l7_vag_readblock(struct diag_l2_conn *d_l2_conn, char block_no, char* block_out)
{
	int errval = 0;
	struct diag_msg *msg = diag_allocmsg(1);
	struct diag_msg *resp = NULL;
	//struct diag_msg *resp_tmp = NULL;
	//int count = 0;

	msg->type = 0x29; // block title -> group reading
	msg->data[0] = block_no;
	msg->len = 1;

	resp = diag_l2_request(global_l2_conn, msg, &errval);

	if (resp == NULL)
		return errval;

	if (resp->len<12 /*|| !success_p(&msg, resp)*/) {
		diag_freemsg(resp);
		diag_freemsg(msg);
		return DIAG_ERR_ECUSAIDNO;
	}

	//if(!buflen) goto NO_ERR;

	//resp_tmp=resp;

	

/*for(int i=0; resp_tmp; i++)
	{
printf("READ %d: ",i);
		for(int j=0; j<resp_tmp->len; j++)
		{
		printf("0X%02X ",resp_tmp->data[j]);

		}

	printf("\n");		
		resp_tmp=resp_tmp->next;
	}*/

	strncpy(block_out,resp->data,12);

	diag_freemsg(resp);
	diag_freemsg(msg);
	//return read_family(argc, argv, NS_LIVEDATA);
	return 0;
}

/*
 * Retrieve list of stored DTCs.
 */
int
//diag_l7_vag_dtclist(struct diag_l2_conn *d_l2_conn, int buflen, uint8_t *out)
diag_l7_vag_dtclist(struct diag_l2_conn *d_l2_conn, int buflen, struct vw_dtc_data *out)
{
	//uint8_t req[] = {0x07, 0x00};//{ readDiagnosticTroulbleCodes, 1 };
	int errval = 0;
	struct diag_msg msg = {0};
	struct diag_msg *resp = NULL;
	struct diag_msg *resp_tmp = NULL;
	int count = 0;

	msg.type = 0x07;
	msg.data = NULL;
	msg.len = 0;

	resp = diag_l2_request(d_l2_conn, &msg, &errval);

	if (resp == NULL)
		return errval;

	if (resp->len<3 /*|| !success_p(&msg, resp)*/) {
		diag_freemsg(resp);
		return DIAG_ERR_ECUSAIDNO;
	}

	if(!buflen) goto NO_ERR;

	resp_tmp=resp;
	for(int i=0; resp_tmp; i++)
	{

		if (resp_tmp->len%3) 
		{
			diag_freemsg(resp);
			return DIAG_ERR_ECUSAIDNO;
		}


		for(int j=0; j<resp_tmp->len; j=j+3)
		{
			out[count].value= (resp_tmp->data[j] <<8) + resp_tmp->data[j+1];
			out[count].status= resp_tmp->data[j+2];
			count++;

			if(count> buflen)
			{
				fprintf(stderr, "Warning: retrieving only first %d DTCs\n",buflen);
				goto NO_ERR;						
			}

		}

		printf("\n");
		resp_tmp=resp_tmp->next;
	}

NO_ERR:

	diag_freemsg(resp);
	return count;
}

/*
 * Attempt to clear stored DTCs.
 *
 * Returns 0 if there were no DTCs, 1 if there was at least one DTC and the
 * ECU returned positive acknowledgement for the clear request, <0 for errors.
 */
int
diag_l7_vag_cleardtc(struct diag_l2_conn *d_l2_conn)
{
	struct diag_msg msg = {0};
	struct diag_msg *resp = NULL;
	int rv;

	/*
	 * ECU will reject clearDiagnosticInformation unless preceded by
	 * readDiagnosticTroubleCodes.
	 */
	rv = diag_l7_vag_dtclist(d_l2_conn,0,NULL);
	if (rv < 0)
		return rv;

	msg.type = 0x05;
	msg.data = NULL;
	msg.len = 0;
	resp = diag_l2_request(d_l2_conn, &msg, &rv);


	if (resp == NULL)
		return rv;

	if (resp->type==0x09) {
		diag_freemsg(resp);
		return 1;
	} else {
		diag_freemsg(resp);
		return DIAG_ERR_ECUSAIDNO;
	}
}
