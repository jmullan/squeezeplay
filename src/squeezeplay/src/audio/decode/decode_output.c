/*
** Copyright 2007-2008 Logitech. All Rights Reserved.
**
** This file is subject to the Logitech Public Source License Version 1.0. Please see the LICENCE file for details.
*/

//#define RUNTIME_DEBUG 1

#include "common.h"

#include "audio/fifo.h"
#include "audio/fixed_math.h"
#include "audio/decode/decode.h"
#include "audio/decode/decode_priv.h"


/* The start point of the last track */
static bool_t check_start_point = FALSE;
static size_t track_start_point = 0;

/* Has the audio output been initialized? */
static bool_t output_started = FALSE;

/* Track transition information */
static u32_t decode_transition_type = 0;
static u32_t decode_transition_period = 0;

static bool_t crossfade_started;
static size_t crossfade_ptr;
static fft_fixed transition_gain;
static fft_fixed transition_gain_step;
static u32_t transition_sample_step;
static u32_t transition_samples_in_step;


#define TRANSITION_NONE         0x0
#define TRANSITION_CROSSFADE    0x1
#define TRANSITION_FADE_IN      0x2
#define TRANSITION_FADE_OUT     0x4

/* Transition steps per second should be a common factor
 * of all supported sample rates.
 */
#define TRANSITION_STEPS_PER_SECOND 10
#define TRANSITION_MINIMUM_SECONDS 1


void decode_output_begin(void) {
	// XXXX fifo mutex

	decode_audio->start();

	if (output_started) {
		return;
	}

	output_started = TRUE;

	decode_fifo.rptr = 0;
	decode_fifo.wptr = 0;
}


void decode_output_end(void) {
	// XXXX fifo mutex

	output_started = FALSE;

	decode_audio->stop();
}


void decode_output_flush(void) {
	// XXXX fifo mutex

	if (check_start_point) {
		decode_fifo.wptr = track_start_point;
	}
	else {
		decode_fifo.rptr = decode_fifo.wptr;

		/* abort audio playback */
		decode_audio->stop();
	}
}


bool_t decode_check_start_point(void) {
	bool_t reached_start_point;

	if (!check_start_point) {
		/* We are past the start point */
		return false;
	}

	/* We mark the start point of a track in the decode FIFO. This function
	 * tells us whether we've played past that point.
	 */
	if (decode_fifo.wptr > track_start_point) {
		reached_start_point = ((decode_fifo.rptr > track_start_point) &&
			(decode_fifo.rptr <= decode_fifo.wptr));
	}
	else {
		reached_start_point = ((decode_fifo.rptr > track_start_point) ||
			(decode_fifo.rptr <= decode_fifo.wptr));
	}

	if (!reached_start_point) {
		/* We have not reached the start point */
		return false;
	}

	/* Past the start point */
	check_start_point = FALSE;
	decode_num_tracks_started++;
	decode_elapsed_samples = 0;

	return true;
}


/* Determine whether we have enough audio in the output buffer to do
 * a transition. Start at the requested transition interval and go
 * down till we find an interval that we have enough audio for.
 */
static fft_fixed determine_transition_interval(int sample_rate, u32_t transition_period, u32_t *nbytes) {
	u32_t bytes_used, sample_step_bytes;
	fft_fixed interval, interval_step;

	bytes_used = fifo_bytes_used(&decode_fifo);
	*nbytes = SAMPLES_TO_BYTES(TRANSITION_MINIMUM_SECONDS * sample_rate);
	if (bytes_used < *nbytes) {
		return 0;
	}

	*nbytes = SAMPLES_TO_BYTES(transition_period * sample_rate);
	transition_sample_step = sample_rate / TRANSITION_STEPS_PER_SECOND;
	sample_step_bytes = SAMPLES_TO_BYTES(transition_sample_step);

	interval = s32_to_fixed(transition_period);
	interval_step = fixed_div(FIXED_ONE, TRANSITION_STEPS_PER_SECOND);

	while (bytes_used < (*nbytes + sample_step_bytes)) {
		*nbytes -= sample_step_bytes;
		interval -= interval_step;
	}

	return interval;
}

/* How many bytes till we're done with the transition.
 */
static size_t decode_transition_bytes_remaining() {
	return (crossfade_ptr >= decode_fifo.wptr) ? (crossfade_ptr - decode_fifo.wptr) : (decode_fifo.wptr - crossfade_ptr + decode_fifo.size);
}


/* Called to copy samples to the decode fifo when we are doing
 * a transition - crossfade or fade in. This method applies gain
 * to both the new signal and the one that's already in the fifo.
 */
static void decode_transition_copy_bytes(sample_t *buffer, int nbytes) {
	sample_t chunk[nbytes * sizeof(sample_t)];
	sample_t *chunk_ptr = chunk;
	sample_t sample;
	int nsamples, s;

	// XXXX process in smaller buffers, of size transition_samples_in_step

	nsamples = BYTES_TO_SAMPLES(nbytes);

	if (crossfade_started) {
		memcpy(chunk, decode_fifo_buf + decode_fifo.wptr, nbytes);
	}
	else {
		memset(chunk, 0, nbytes);
	}

	fft_fixed in_gain = transition_gain;
	fft_fixed out_gain = FIXED_ONE - in_gain;
	for (s=0; s<nsamples * 2; s++) {
		
		sample = fixed_mul(out_gain, *chunk_ptr);
		sample += fixed_mul(in_gain, *buffer++);
		*chunk_ptr++ = sample;
	}

	transition_samples_in_step += nsamples;
	while (transition_samples_in_step >= transition_sample_step) {
		transition_samples_in_step -= transition_sample_step;
		transition_gain += transition_gain_step;
	}

	memcpy(decode_fifo_buf + decode_fifo.wptr, chunk, nbytes);
	fifo_wptr_incby(&decode_fifo, nbytes);
}


void decode_output_samples(sample_t *buffer, u32_t nsamples, int sample_rate,
			   bool_t need_scaling, bool_t start_immediately,
			   bool_t copyright_asserted) {
	size_t bytes_out;

	DEBUG_TRACE("Got %d samples\n", samples);

	/* Some decoders can pass no samples at the start of the track. Stop
	 * early, otherwise we may send the track start event at the wrong
	 * time.
	 */
	if (nsamples == 0) {
		return;
	}

	// XXXX full port from ip3k

	fifo_lock(&decode_fifo);

	if (decode_first_buffer) {
		crossfade_started = FALSE;
		track_start_point = decode_fifo.wptr;
		
		if (decode_transition_type & TRANSITION_CROSSFADE) {
			u32_t crossfadeBytes;

			/* We are being asked to do a crossfade. Find out
			 * if it is possible.
			 */
			fft_fixed interval = determine_transition_interval(sample_rate, decode_transition_period, &crossfadeBytes);

			if (interval) {
				printf("Starting CROSSFADE over %d seconds, requiring %d bytes\n", fixed_to_s32(interval), crossfadeBytes);

				/* Buffer position to stop crossfade */
				crossfade_ptr = decode_fifo.wptr;

				/* Buffer position to start crossfade */
				decode_fifo.wptr = (crossfadeBytes <= decode_fifo.wptr) ? (decode_fifo.wptr - crossfadeBytes) : (decode_fifo.wptr - crossfadeBytes + decode_fifo.size);

				/* Gain steps */
				transition_gain_step = fixed_div(FIXED_ONE, fixed_mul(interval, s32_to_fixed(TRANSITION_STEPS_PER_SECOND)));
				transition_gain = 0;
				transition_samples_in_step = 0;

				crossfade_started = TRUE;
				track_start_point = decode_fifo.wptr;
			}
			/* 
			 * else there aren't enough leftover samples from the
			 * previous track, so abort the transition.
			 */
		}
		else if (decode_transition_type & TRANSITION_FADE_IN) {
			u32_t transition_period;

			/* The transition is a fade in. */

			transition_period = decode_transition_period;

			/* Halve the period if we're also fading out */
			if (decode_transition_type & TRANSITION_FADE_OUT) {
				transition_period >>= 1;
			}

			printf("Starting FADE_IN over %d seconds\n", transition_period);

			/* Gain steps */
			transition_gain_step = fixed_div(FIXED_ONE, s32_to_fixed(transition_period * TRANSITION_STEPS_PER_SECOND));
			transition_gain = 0;
			transition_sample_step = sample_rate / TRANSITION_STEPS_PER_SECOND;
			transition_samples_in_step = 0;
		}

		current_sample_rate = sample_rate;

		check_start_point = TRUE;
		decode_first_buffer = FALSE;
	}

	bytes_out = SAMPLES_TO_BYTES(nsamples);

	while (bytes_out) {
		size_t wrap, bytes_write, bytes_remaining;

		/* The size of the output write is limied by the
		 * space untill our fifo wraps.
		 */
		wrap = fifo_bytes_until_wptr_wrap(&decode_fifo);

		/* When crossfading limit the output write to the
		 * end of the transition.
		 */
		if (crossfade_started) {
			bytes_remaining = decode_transition_bytes_remaining();
printf("bytes_till_end=%d\n", bytes_remaining);

			if (bytes_remaining < wrap) {
				wrap = bytes_remaining;
			}
		}

		bytes_write = bytes_out;
		if (bytes_write > wrap) {
			bytes_write = wrap;
		}

		if (transition_gain_step) {
			decode_transition_copy_bytes(buffer, bytes_write);

			if ((crossfade_started && decode_fifo.wptr == crossfade_ptr)
			    || transition_gain >= FIXED_ONE) {
				printf("Completed transition\n");

				transition_gain_step = 0;
				crossfade_started = FALSE;
			}
		}
		else {
			memcpy(decode_fifo_buf + decode_fifo.wptr, buffer, bytes_write);
			fifo_wptr_incby(&decode_fifo, bytes_write);
		}

		buffer += (bytes_write / sizeof(sample_t));
		bytes_out -= bytes_write;
	}

	if (start_immediately) {
		current_audio_state = DECODE_STATE_RUNNING;
	}

	fifo_unlock(&decode_fifo);
}


// XXXX is this really buffer_size, or number_samples?
bool_t decode_output_can_write(u32_t buffer_size, u32_t sample_rate) {
	size_t freebytes;

	// XXXX full port from ip3k
	
	fifo_lock(&decode_fifo);

	freebytes = fifo_bytes_free(&decode_fifo);

	fifo_unlock(&decode_fifo);

	if (freebytes >= buffer_size) {
		return TRUE;
	}

	return FALSE;
}


/* This removes padding samples from the buffer (for gapless mp3 playback). */
void decode_output_remove_padding(u32_t nsamples, u32_t sample_rate) {
#if 0
	int numerator, denominator;
	u32_t resampled_rate;
#endif
	size_t buffer_size;

	buffer_size = SAMPLES_TO_BYTES(nsamples);

#if 0
	// XXXX full port from ip3k
	u32_t resampled_rate = decode_output_scaled_samplerate(sample_rate, &numerator, &denominator);
	if (numerator != 1) {
		buffer_size /= numerator;
	}
	buffer_size *= denominator;
#endif

	DEBUG_TRACE("Removing %d bytes padding from buffer", buffer_size);

	fifo_lock(&decode_fifo);

	/* have we already started playing the padding? */
	if (fifo_bytes_used(&decode_fifo) <= buffer_size) {
		fifo_unlock(&decode_fifo);

		DEBUG_TRACE("- already playing padding");
		return;
	}

	if (decode_fifo.wptr < buffer_size) {
		decode_fifo.wptr += decode_fifo.size - buffer_size;
	}
	else {
		decode_fifo.wptr -= buffer_size;
	}

	fifo_unlock(&decode_fifo);
}


int decode_output_samplerate() {
	return current_sample_rate;
}


void decode_set_transition(u32_t type, u32_t period) {
	if (!period) {
		decode_transition_type = TRANSITION_NONE;
		return;
	}

	switch (type - '0') {
	case 0:
		decode_transition_type = TRANSITION_NONE;
		break;
	case 1:
		decode_transition_type = TRANSITION_CROSSFADE;
		break;
	case 2:
		decode_transition_type = TRANSITION_FADE_IN;
		break;
	case 3:
		decode_transition_type = TRANSITION_FADE_OUT;
		break;
	case 4:
		decode_transition_type = TRANSITION_FADE_IN | TRANSITION_FADE_OUT;
		break;
	}

	decode_transition_period = period;
}
