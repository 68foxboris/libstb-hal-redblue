/*
 * linuxdvb output/writer handling.
 *
 * konfetti 2010 based on linuxdvb.c code from libeplayer2
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

/* ***************************** */
/* Includes		      */
/* ***************************** */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <linux/dvb/video.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/stm_ioctls.h>
#include <memory.h>
#include <asm/types.h>
#include <pthread.h>
#include <errno.h>

#include "common.h"
#include "output.h"
#include "debug.h"
#include "misc.h"
#include "pes.h"
#include "writer.h"

/* ***************************** */
/* Makros/Constants	      */
/* ***************************** */

#define WMV3_PRIVATE_DATA_LENGTH			4

#define METADATA_STRUCT_A_START	     12
#define METADATA_STRUCT_B_START	     24
#define METADATA_STRUCT_B_FRAMERATE_START   32
#define METADATA_STRUCT_C_START	     8


#define VC1_SEQUENCE_LAYER_METADATA_START_CODE	  0x80
#define VC1_FRAME_START_CODE			    0x0d

#define VC1_DEBUG

#ifdef VC1_DEBUG

static short debug_level = 0;

#define vc1_printf(level, fmt, x...) do { \
if (debug_level >= level) printf("[%s:%s] " fmt, __FILE__, __FUNCTION__, ## x); } while (0)
#else
#define vc1_printf(level, fmt, x...)
#endif

#ifndef VC1_SILENT
#define vc1_err(fmt, x...) do { printf("[%s:%s] " fmt, __FILE__, __FUNCTION__, ## x); } while (0)
#else
#define vc1_err(fmt, x...)
#endif


/* ***************************** */
/* Types			 */
/* ***************************** */

static const unsigned char SequenceLayerStartCode[] =
    { 0x00, 0x00, 0x01, VC1_SEQUENCE_LAYER_METADATA_START_CODE };


static const unsigned char Metadata[] = {
    0x00, 0x00, 0x00, 0xc5,
    0x04, 0x00, 0x00, 0x00,
    0xc0, 0x00, 0x00, 0x00,	/* Struct C set for for advanced profile */
    0x00, 0x00, 0x00, 0x00,	/* Struct A */
    0x00, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x00, 0x00,
    0x60, 0x00, 0x00, 0x00,	/* Struct B */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

/* ***************************** */
/* Varaibles                     */
/* ***************************** */
static int initialHeader = 1;
static unsigned char FrameHeaderSeen = 0;

/* ***************************** */
/* Prototypes                    */
/* ***************************** */

/* ***************************** */
/* MISC Functions                */
/* ***************************** */
static int reset()
{
    initialHeader = 1;
    FrameHeaderSeen = 0;
    return 0;
}

static int writeData(WriterAVCallData_t *call)
{
    int len = 0;

    if (call->fd < 0) {
	vc1_err("file pointer < 0. ignoring ...\n");
	return 0;
    }

    vc1_printf(10, "VideoPts %lld\n", call->Pts);

    vc1_printf(10, "Got Private Size %d\n", call->stream->codec->extradata_size);


    if (initialHeader) {

	unsigned char PesHeader[PES_MAX_HEADER_SIZE];
	unsigned char PesPayload[128];
	unsigned char *PesPtr;
	unsigned int crazyFramerate = 0;
	struct iovec iov[2];

	vc1_printf(10, "Framerate: %f\n", 1000.0 * av_q2d(call->stream->r_frame_rate));
	vc1_printf(10, "biWidth: %d\n", call->stream->codec->width);
	vc1_printf(10, "biHeight: %d\n", call->stream->codec->height);

	crazyFramerate = ((10000000.0 / av_q2d(call->stream->r_frame_rate)));

	vc1_printf(10, "crazyFramerate: %u\n", crazyFramerate);

	memset(PesPayload, 0, sizeof(PesPayload));

	PesPtr = PesPayload;

	memcpy(PesPtr, SequenceLayerStartCode,
	       sizeof(SequenceLayerStartCode));
	PesPtr += sizeof(SequenceLayerStartCode);

	memcpy(PesPtr, Metadata, sizeof(Metadata));
	PesPtr += METADATA_STRUCT_C_START;
	PesPtr += WMV3_PRIVATE_DATA_LENGTH;

	/* Metadata Header Struct A */
	*PesPtr++ = (call->stream->codec->height >> 0) & 0xff;
	*PesPtr++ = (call->stream->codec->height >> 8) & 0xff;
	*PesPtr++ = (call->stream->codec->height >> 16) & 0xff;
	*PesPtr++ = call->stream->codec->height >> 24;
	*PesPtr++ = (call->stream->codec->width >> 0) & 0xff;
	*PesPtr++ = (call->stream->codec->width >> 8) & 0xff;
	*PesPtr++ = (call->stream->codec->width >> 16) & 0xff;
	*PesPtr++ = call->stream->codec->width >> 24;

	PesPtr += 12;		/* Skip flag word and Struct B first 8 bytes */

	*PesPtr++ = (crazyFramerate >> 0) & 0xff;
	*PesPtr++ = (crazyFramerate >> 8) & 0xff;
	*PesPtr++ = (crazyFramerate >> 16) & 0xff;
	*PesPtr++ = crazyFramerate >> 24;

	iov[0].iov_base = PesHeader;
	iov[1].iov_base = PesPayload;
	iov[1].iov_len = PesPtr - PesPayload;
	iov[0].iov_len =
	    InsertPesHeader(PesHeader, iov[1].iov_len,
			    VC1_VIDEO_PES_START_CODE, INVALID_PTS_VALUE,
			    0);
	len = writev(call->fd, iov, 2);

	/* For VC1 the codec private data is a standard vc1 sequence header so we just copy it to the output */
	iov[0].iov_base = PesHeader;
	iov[1].iov_base = call->stream->codec->extradata;
	iov[1].iov_len = call->stream->codec->extradata_size;
	iov[0].iov_len =
	    InsertPesHeader(PesHeader, iov[1].iov_len,
			    VC1_VIDEO_PES_START_CODE, INVALID_PTS_VALUE,
			    0);
	len = writev(call->fd, iov, 2);

	initialHeader = 0;
    }

    if (call->packet->size > 0 && call->packet->data) {
	int Position = 0;
	unsigned char insertSampleHeader = 1;

	while (Position < call->packet->size) {

	    int PacketLength =
		(call->packet->size - Position) <=
		MAX_PES_PACKET_SIZE ? (call->packet->size -
				       Position) : MAX_PES_PACKET_SIZE;

	    int Remaining = call->packet->size - Position - PacketLength;

	    vc1_printf(20, "PacketLength=%d, Remaining=%d, Position=%d\n",
		       PacketLength, Remaining, Position);

	    unsigned char PesHeader[PES_MAX_HEADER_SIZE];
	    int HeaderLength =
		InsertPesHeader(PesHeader, PacketLength,
				VC1_VIDEO_PES_START_CODE, call->Pts, 0);

	    if (insertSampleHeader) {
		const unsigned char Vc1FrameStartCode[] =
		    { 0, 0, 1, VC1_FRAME_START_CODE };

/*
		    vc1_printf(10, "Data Start: {00 00 01 0d} - ");
		    int i;
		    for (i = 0; i < 4; i++) vc1_printf(10, "%02x ", call->packet->data[i]);
		    vc1_printf(10, "\n");
*/

		if (!FrameHeaderSeen && (call->packet->size > 3)
		    && (memcmp(call->packet->data, Vc1FrameStartCode, 4) == 0))
		    FrameHeaderSeen = 1;
		if (!FrameHeaderSeen) {
		    memcpy(&PesHeader[HeaderLength], Vc1FrameStartCode,
			   sizeof(Vc1FrameStartCode));
		    HeaderLength += sizeof(Vc1FrameStartCode);
		}
		insertSampleHeader = 0;
	    }

	    struct iovec iov[2];
	    iov[0].iov_base = PesHeader;
	    iov[0].iov_len = HeaderLength;
	    iov[1].iov_base = call->packet->data + Position;
	    iov[1].iov_len = PacketLength;

	    ssize_t l = writev(call->fd, iov, 2);
	    if (l < 0) {
		len = l;
		break;
	    }
	    len += l;

	    Position += PacketLength;
	    call->Pts = INVALID_PTS_VALUE;
	}
    }

    vc1_printf(10, "< %d\n", len);
    return len;
}

/* ***************************** */
/* Writer  Definition	    */
/* ***************************** */

static WriterCaps_t caps = {
    "vc1",
    eVideo,
    "V_VC1",
    VIDEO_ENCODING_VC1
};

struct Writer_s WriterVideoVC1 = {
    &reset,
    &writeData,
    NULL,
    &caps
};
