/***************************************************************************
 *   Copyright (C) 2005 to 2007 by Jonathan Duddington                     *
 *   email: jonsd@users.sourceforge.net                                    *
 *   Copyright (C) 2013-2015 Reece H. Dunn                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write see:                           *
 *               <http://www.gnu.org/licenses/>.                           *
 ***************************************************************************/

#include "speak_lib.h"
#include "speech.h"
#include "phoneme.h"
#include "synthesize.h"
#include "voice.h"
#include "spect.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern double ConvertFromIeeeExtended(unsigned char *bytes);
extern int PeaksToHarmspect(wavegen_peaks_t *peaks, int pitch, int *htab, int control);

extern unsigned char pk_shape1[];
extern int pk_select;
extern char voice_name[];

static int frame_width;
int pk_select;

#define DRAWPEAKWIDTH 2000
#define PEAKSHAPEW 256

static int default_freq[N_PEAKS] =
{200,500,1200,3000,3500,4000,6900,7800,9000};
static int default_width[N_PEAKS] =
{750,500,550,550,600,700,700,700,700};
static int default_klt_bw[N_PEAKS] =
{89,90,140,260,260,260,500,500,500};

static double read_double(FILE *stream)
{
	unsigned char bytes[10];
	fread(bytes,sizeof(char),10,stream);
	return ConvertFromIeeeExtended(bytes);
}

float polint(float xa[],float ya[],int n,float x)
{
// General polinomial interpolation routine, xa[1...n] ya[1...n]
	int i,m,ns=1;
	float den,dif,dift,ho,hp,w;
	float y;  // result
	float c[9],d[9];

	dif=fabs(x-xa[1]);

	for(i=1; i<=n; i++) {
		if((dift=fabs(x-xa[i])) < dif) {
			ns=i;
			dif=dift;
		}
		c[i]=ya[i];
		d[i]=ya[i];
	}
	y=ya[ns--];
	for(m=1; m<n; m++) {
		for(i=1; i<=n-m; i++) {
			ho=xa[i]-x;
			hp=xa[i+m]-x;
			w=c[i+1]-d[i];
			if((den=ho-hp) == 0.0)
			{
				return(ya[2]);  // two input xa are identical
			}
			den=w/den;
			d[i]=hp*den;
			c[i]=ho*den;
		}
		y += ((2*ns < (n-m) ? c[ns+1] : d[ns--]));
	}
	return(y);
}


static void PeaksZero(peak_t *sp, peak_t *zero)
{
	int pk;

	memcpy(zero,sp,sizeof(peak_t)*N_PEAKS);

	for(pk=0; pk<N_PEAKS; pk++)
		zero[pk].pkheight = 0;

}

static SpectFrame *SpectFrameCreate()
{
	int ix;
	SpectFrame *frame;

	frame = malloc(sizeof(SpectFrame));
	frame->keyframe = 0;
	frame->spect = NULL;
	frame->markers = 0;
	frame->pitch = 0;
	frame->nx = 0;
	frame->time = 0;
	frame->length = 0;
	frame->amp_adjust = 100;
	frame->length_adjust = 0;

	for(ix=0; ix<N_PEAKS; ix++)
	{
		frame->formants[ix].freq = 0;
		frame->peaks[ix].pkfreq = default_freq[ix];
		frame->peaks[ix].pkheight = 0;
		frame->peaks[ix].pkwidth = default_width[ix];
		frame->peaks[ix].pkright = default_width[ix];
		frame->peaks[ix].klt_bw = default_klt_bw[ix];
		frame->peaks[ix].klt_ap = 0;
		frame->peaks[ix].klt_bp = default_klt_bw[ix];
	}

	memset(frame->klatt_param, 0, sizeof(frame->klatt_param));
	frame->klatt_param[KLATT_AV] = 59;
	frame->klatt_param[KLATT_Kopen] = 40;

	return frame;
}

static void SpectFrameDestroy(SpectFrame *frame)
{
	if(frame->spect != NULL)
		free(frame->spect);
	free(frame);
}



int LoadFrame(SpectFrame *frame, FILE *stream, int file_format_type)
{
	short ix;
	short x;
	unsigned short *spect_data;

	frame->time = read_double(stream);
	frame->pitch = read_double(stream);
	frame->length = read_double(stream);
	frame->dx = read_double(stream);
	fread(&frame->nx,sizeof(short),1,stream);
	fread(&frame->markers,sizeof(short),1,stream);
	fread(&frame->amp_adjust,sizeof(short),1,stream);

	if(file_format_type == 2)
	{
		fread(&ix,sizeof(short),1,stream); // spare
		fread(&ix,sizeof(short),1,stream); // spare
	}

	for(ix=0; ix<N_PEAKS; ix++)
	{
		fread(&frame->formants[ix].freq,sizeof(short),1,stream);
		fread(&frame->formants[ix].bandw,sizeof(short),1,stream);
		fread(&frame->peaks[ix].pkfreq,sizeof(short),1,stream);
		fread(&frame->peaks[ix].pkheight,sizeof(short),1,stream);
		fread(&frame->peaks[ix].pkwidth,sizeof(short),1,stream);
		fread(&frame->peaks[ix].pkright,sizeof(short),1,stream);
		if(frame->peaks[ix].pkheight > 0)
			frame->keyframe = 1;

		if(file_format_type == 2)
		{
			fread(&frame->peaks[ix].klt_bw,sizeof(short),1,stream);
			fread(&frame->peaks[ix].klt_ap,sizeof(short),1,stream);
			fread(&frame->peaks[ix].klt_bp,sizeof(short),1,stream);
		}
	}

	if(file_format_type > 0)
	{
		for(ix=0; ix<N_KLATTP2; ix++)
		{
			fread(frame->klatt_param + ix,sizeof(short),1,stream);
		}
	}

	spect_data = malloc(sizeof(USHORT) * frame->nx);

	if(spect_data == NULL)
	{
		fprintf(stderr,"Failed to allocate memory\n");
		return(1);
	}

	frame->max_y = 0;
	for(ix=0; ix<frame->nx; ix++)
	{
		fread(&x,sizeof(short),1,stream);
		spect_data[ix] = x;
		if(x > frame->max_y) frame->max_y = x;
	}
	frame->spect = spect_data;

	return(0);
}



double GetFrameRms(SpectFrame *frame, int seq_amplitude)
{
	int h;
	float total=0;
	int maxh;
	int height;
	int htab[400];
	wavegen_peaks_t wpeaks[9];

	for(h=0; h<9; h++)
	{
		height = (frame->peaks[h].pkheight * seq_amplitude * frame->amp_adjust)/10000;
		wpeaks[h].height = height << 8;

		wpeaks[h].freq = frame->peaks[h].pkfreq << 16;
		wpeaks[h].left = frame->peaks[h].pkwidth << 16;
		wpeaks[h].right = frame->peaks[h].pkright << 16;
	}

	maxh = PeaksToHarmspect(wpeaks,90<<16,htab,0);
	for(h=1; h<maxh; h++)
	{
		total += ((htab[h] * htab[h]) >> 10);
	}
	frame->rms = sqrt(total) / 7.25;
	return(frame->rms);
}


SpectSeq *SpectSeqCreate()
{
	SpectSeq *spect = malloc(sizeof(SpectSeq));

	spect->numframes = 0;
	spect->frames = NULL;
	spect->name = NULL;

	pk_select = 1;
	spect->grid = 1;
	spect->duration = 0;
	spect->pitch1 = 0;
	spect->pitch2 = 0;
	spect->bass_reduction = 0;

	spect->max_x = 3000;
	spect->max_y = 1;
	spect->file_format = 0;

	return spect;
}

void SpectSeqDestroy(SpectSeq *spect)
{
	int ix;
	if(spect->frames != NULL)
	{
		for(ix=0; ix<spect->numframes; ix++)
		{
			if(spect->frames[ix] != NULL)
				SpectFrameDestroy(spect->frames[ix]);
		}
		free(spect->frames);
	}
	free(spect->name);
	free(spect);
}


static float GetFrameLength(SpectSeq *spect, int frame)
{
	int ix;
	float adjust=0;

	if(frame >= spect->numframes-1) return(0);

	for(ix=frame+1; ix<spect->numframes-1; ix++)
	{
		if(spect->frames[ix]->keyframe) break;  // reached next keyframe
		adjust += spect->frames[ix]->length_adjust;
	}
	return ((spect->frames[ix]->time - spect->frames[frame]->time) * 1000.0 + adjust);
}



int LoadSpectSeq(SpectSeq *spect, const char *filename)
{
	short n, temp;
	int ix;
	uint32_t id1, id2, name_len;
	int set_max_y=0;
	float time_offset;

	FILE *stream = fopen(filename, "rb");
	if(stream == NULL)
	{
		fprintf(stderr, "Failed to open: '%s'", filename);
		return(0);
	}

	fread(&id1,sizeof(uint32_t),1,stream);
	fread(&id2,sizeof(uint32_t),1,stream);

	if((id1 == FILEID1_SPECTSEQ) && (id2 == FILEID2_SPECTSEQ))
	{
		spect->file_format = 0;       // eSpeak formants
	}
	else
	if((id1 == FILEID1_SPECTSEQ) && (id2 == FILEID2_SPECTSEK))
	{
		spect->file_format = 1;       // formants for Klatt synthesizer
	}
	else
	if((id1 == FILEID1_SPECTSEQ) && (id2 == FILEID2_SPECTSQ2))
	{
		spect->file_format = 2;       // formants for Klatt synthesizer
	}
	else
	{
		fprintf(stderr, "Unsupported spectral file format.\n");
		return(1);
	}

	fread(&name_len,sizeof(uint32_t),1,stream);
	if (name_len > 0)
	{
		spect->name = (char *)malloc(name_len);
		fread(spect->name,sizeof(char),name_len,stream);
	}
	else
		spect->name = NULL;

	fread(&n,sizeof(short),1,stream);
	fread(&spect->amplitude,sizeof(short),1,stream);
	fread(&spect->max_y,sizeof(short),1,stream);
	fread(&temp,sizeof(short),1,stream); // unused

	if(n==0) return(0);

	if(spect->frames != NULL)
	{
		for(ix=0; ix<spect->numframes; ix++)
		{
			if(spect->frames[ix] != NULL)
				SpectFrameDestroy(spect->frames[ix]);
		}
		free(spect->frames);
	}
	spect->frames = malloc(sizeof(SpectFrame) * n);

	spect->numframes = 0;
	spect->max_x = 3000;
	if(spect->max_y == 0)
	{
		set_max_y = 1;
		spect->max_y = 1;
	}
	for(ix = 0; ix < n; ix++)
	{
		SpectFrame *frame = SpectFrameCreate();

		if(LoadFrame(frame, stream, spect->file_format) != 0)
		{
			free(frame);
			break;
		}

		spect->frames[spect->numframes++] = frame;

		if(set_max_y && (frame->max_y > spect->max_y))
			spect->max_y = frame->max_y;
		if(frame->nx * frame->dx > spect->max_x) spect->max_x = (int)(frame->nx * frame->dx);
	}
	spect->max_x = 9000; // disable auto-xscaling

	frame_width = (int)((FRAME_WIDTH*spect->max_x)/MAX_DISPLAY_FREQ);
	if(frame_width > FRAME_WIDTH) frame_width = FRAME_WIDTH;


	// start times from zero
	time_offset = spect->frames[0]->time;
	for(ix=0; ix<spect->numframes; ix++)
		spect->frames[ix]->time -= time_offset;

	spect->pitch1 = spect->pitchenv.pitch1;
	spect->pitch2 = spect->pitchenv.pitch2;
	spect->duration = (int)(spect->frames[spect->numframes-1]->time * 1000);

	if(spect->max_y < 400)
		spect->max_y = 200;
	else
		spect->max_y = 29000; // disable auto height scaling

	for(ix=0; ix<spect->numframes; ix++)
	{
		if(spect->frames[ix]->keyframe)
			spect->frames[ix]->length_adjust = spect->frames[ix]->length - GetFrameLength(spect,ix);
	}
	return(0);
}
