
/*
 * August 24, 1998
 * Copyright (C) 1998 Juergen Mueller And Sundry Contributors
 * This source code is freely redistributable and may be used for
 * any purpose.  This copyright notice must be maintained. 
 * Lance Norskog And Sundry Contributors are not responsible for 
 * the consequences of using this software.
 */

/*
** 	Echo effect. based on:
**
** echoplex.c - echo generator
**
** Copyright (C) 1989 by Jef Poskanzer.
**
** Permission to use, copy, modify, and distribute this software and its
** documentation for any purpose and without fee is hereby granted, provided
** that the above copyright notice appear in all copies and that both that
** copyright notice and this permission notice appear in supporting
** documentation.  This software is provided "as is" without express or
** implied warranty.
*/


/*
 * Changes to old "echo.c" now called "reverb.c":
 *
 * The effect name changes from "echo" to "reverb" (see Guitar FX FAQ) for
 * the difference in its defintion.
 * The idea of the echoplexer is modified and enhanceb by an automatic
 * setting of each decay for realistic reverb.
 * Some bugs are fixed concerning malloc and fade-outs.
 * Added an output volume (gain-out) avoiding saturation or clipping.
 *
 *
 * Reverb effect for dsp.
 *
 * Flow diagram scheme for n delays ( 1 <= n <= MAXREVERB )
 *
 *        * gain-in  +---+                        * gain-out
 * ibuff ----------->|   |------------------------------------> obuff
 *                   |   |  * decay 1
 *                   |   |<------------------------+
 *                   | + |  * decay 2              |
 *                   |   |<--------------------+   |
 *                   |   |  * decay n          |   |
 *                   |   |<----------------+   |   |
 *                   +---+                 |   |   |
 *                     |      _________    |   |   |
 *                     |     |         |   |   |   |
 *                     +---->| delay n |---+   |   |
 *                     .     |_________|       |   |
 *                     .                       |   |
 *                     .      _________        |   |
 *                     |     |         |       |   |
 *                     +---->| delay 2 |-------+   |
 *                     |     |_________|           |
 *                     |                           |
 *                     |      _________            |
 *                     |     |         |           |
 *                     +---->| delay 1 |-----------+
 *                           |_________|
 *
 *
 *
 * Usage:
 *   reverb gain-out reverb-time delay-1 [ delay-2 ... delay-n ]
 *
 * Where:
 *   gain-out :  0.0 ...      volume
 *   reverb-time :  > 0.0 msec
 *   delay-1 ... delay-n :  > 0.0 msec
 *
 * Note:
 *   gain-in is automatically adjusted avoiding saturation and clipping of
 *   the output. decay-1 to decay-n are computed such that at reverb-time
 *   the input will be 60 dB of the original input for the given delay-1 
 *   to delay-n. delay-1 to delay-n specify the time when the first bounce
 *   of the input will appear. A proper setting for delay-1 to delay-n 
 *   depends on the choosen reverb-time (see hint).
 *
 * Hint:
 *   a realstic reverb effect can be obtained using for a given reverb-time "t"
 *   delays in the range of "t/2 ... t/4". Each delay should not be an integer
 *   of any other.
 *
*/

/*
 * Sound Tools reverb effect file.
 */

#include <stdlib.h> /* Harmless, and prototypes atof() etc. --dgc */
#include <math.h>
#include "st.h"

#define REVERB_FADE_THRESH 10
#define DELAY_BUFSIZ ( 50L * MAXRATE )
#define MAXREVERBS 8

/* Private data for SKEL file */
typedef struct reverbstuff {
	int	counter;			
	int	numdelays;
	float	*reverbbuf;
	float	in_gain, out_gain, time;
	float	delay[MAXREVERBS], decay[MAXREVERBS];
	long	samples[MAXREVERBS], maxsamples;
	LONG	pl, ppl, pppl;
} *reverb_t;

/* Private data for SKEL file */

#ifndef abs
#define abs(a) ((a) >= 0 ? (a) : -(a))
#endif

LONG reverb_clip24(l)
LONG l;
{
	if (l >= ((LONG)1 << 24))
		return ((LONG)1 << 24) - 1;
	else if (l <= -((LONG)1 << 24))
		return -((LONG)1 << 24) + 1;
	else
		return l;
}

/*
 * Process options
 */
void reverb_getopts(effp, n, argv) 
eff_t effp;
int n;
char **argv;
{
	reverb_t reverb = (reverb_t) effp->priv;
	int i;

	reverb->numdelays = 0;
	reverb->maxsamples = 0;

	if ( n < 3 )
	    fail("Usage: reverb gain-out reverb-time delay [ delay ... ]");

	if ( n - 2 > MAXREVERBS )
	    fail("reverb: to many dalays, use less than %i delays",
			MAXREVERBS);

	i = 0;
	sscanf(argv[i++], "%f", &reverb->out_gain);
	sscanf(argv[i++], "%f", &reverb->time);
	while (i < n) {
		/* Linux bug and it's cleaner. */
		sscanf(argv[i++], "%f", &reverb->delay[reverb->numdelays]);
		reverb->numdelays++;
	}
}

/*
 * Prepare for processing.
 */
void reverb_start(effp)
eff_t effp;
{
	reverb_t reverb = (reverb_t) effp->priv;
	int i;

	reverb->in_gain = 1.0;

	if ( reverb->out_gain < 0.0 )
		fail("reverb: gain-out must be positive");
	if ( reverb->out_gain > 1.0 )
		warn("reverb: warnig >>> gain-out can cause saturation of output <<<");
	if ( reverb->time < 0.0 )
		fail("reverb: reverb-time must be positive");
	for(i = 0; i < reverb->numdelays; i++) {
		reverb->samples[i] = reverb->delay[i] * effp->ininfo.rate / 1000.0;
		if ( reverb->samples[i] < 1 )
		    fail("reverb: delay must be positive!\n");
		if ( reverb->samples[i] > DELAY_BUFSIZ )
			fail("reverb: delay must be less than %g seconds!\n",
				DELAY_BUFSIZ / (float) effp->ininfo.rate );
		/* Compute a realistic decay */
		reverb->decay[i] = (float) pow(10.0,(-3.0 * reverb->delay[i] / reverb->time));
		if ( reverb->samples[i] > reverb->maxsamples )
		    reverb->maxsamples = reverb->samples[i];
	}
	if (! (reverb->reverbbuf = (float *) malloc(sizeof (float) * reverb->maxsamples)))
		fail("reverb: Cannot malloc %d bytes!\n", 
			sizeof(float) * reverb->maxsamples);
	for ( i = 0; i < reverb->maxsamples; ++i )
		reverb->reverbbuf[i] = 0.0;
	reverb->pppl = reverb->ppl = reverb->pl = 0x7fffff;		/* fade-outs */
	reverb->counter = 0;
	/* Compute the input volume carefully */
	for ( i = 0; i < reverb->numdelays; i++ )
		reverb->in_gain *= 
			( 1.0 - ( reverb->decay[i] * reverb->decay[i] ));
}

/*
 * Processed signed long samples from ibuf to obuf.
 * Return number of samples processed.
 */

void reverb_flow(effp, ibuf, obuf, isamp, osamp)
eff_t effp;
LONG *ibuf, *obuf;
int *isamp, *osamp;
{
	reverb_t reverb = (reverb_t) effp->priv;
	int len, done;
	int i, j;
	
	float d_in, d_out;
	LONG out;

	i = reverb->counter;
	len = ((*isamp > *osamp) ? *osamp : *isamp);
	for(done = 0; done < len; done++) {
		/* Store delays as 24-bit signed longs */
		d_in = (float) *ibuf++ / 256;
		d_in = d_in * reverb->in_gain;
		/* Mix decay of delay and input as output */
		for ( j = 0; j < reverb->numdelays; j++ )
			d_in +=
reverb->reverbbuf[(i + reverb->maxsamples - reverb->samples[j]) % reverb->maxsamples] * reverb->decay[j];
		d_out = d_in * reverb->out_gain;
		out = reverb_clip24((LONG) d_out);
		*obuf++ = out * 256;
		reverb->reverbbuf[i] = d_in;
		i++;		/* XXX need a % maxsamples here ? */
		i %= reverb->maxsamples;
	}
	reverb->counter = i;
	/* processed all samples */
}

/*
 * Drain out reverb lines. 
 */
void reverb_drain(effp, obuf, osamp)
eff_t effp;
LONG *obuf;
int *osamp;
{
	reverb_t reverb = (reverb_t) effp->priv;
	float d_in, d_out;
	LONG out, l;
	int i, j, done;

	i = reverb->counter;
	done = 0;
	/* drain out delay samples */
	do {
		d_in = 0;
		d_out = 0;
		for ( j = 0; j < reverb->numdelays; ++j )
			d_in += 
reverb->reverbbuf[(i + reverb->maxsamples - reverb->samples[j]) % reverb->maxsamples] * reverb->decay[j];
		d_out = d_in * reverb->out_gain;
		out = reverb_clip24((LONG) d_out);
		obuf[done++] = out * 256;
		reverb->reverbbuf[i] = d_in;
		l = reverb_clip24((LONG) d_in);
		reverb->pppl = reverb->ppl;
		reverb->ppl = reverb->pl;
		reverb->pl = l;
		i++;		/* need a % maxsamples here ? */
		i %= reverb->maxsamples;
	} while((done < *osamp) && 
		((abs(reverb->pl) + abs(reverb->ppl) + abs(reverb->pppl)) > REVERB_FADE_THRESH));
	reverb->counter = i;
	*osamp = done;
}

/*
 * Clean up reverb effect.
 */
void reverb_stop(effp)
eff_t effp;
{
	reverb_t reverb = (reverb_t) effp->priv;

	free((char *) reverb->reverbbuf);
	reverb->reverbbuf = (float *) -1;   /* guaranteed core dump */
}

