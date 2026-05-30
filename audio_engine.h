/*
 * audio_engine.h — Shared ES8311 + I2S audio engine
 *
 * Runs a FreeRTOS task on Core 0 that continuously services I2S TX/RX.
 * Apps push/pull mono 16-bit samples via lock-free ring buffers.
 * The task handles mono↔stereo conversion and silence on underrun.
 *
 * This decouples audio timing from the Arduino loop, so display flushes
 * and SD card operations can take as long as they need without starving
 * the I2S DMA.
 */
#pragma once
#include <stdint.h>

// Lifecycle — call from app setup, once per boot
void audio_engine_init();
void audio_engine_deinit();

// Playback control (TX path + PA)
void audio_engine_play();        // enable TX drain, PA on
void audio_engine_stop();        // TX goes to silence, PA off

// Recording control (RX path)
void audio_engine_record();      // enable RX capture into ring buffer
void audio_engine_record_stop(); // stop capturing (RX still drained internally)

// Data transfer — all non-blocking, return actual sample count
int  audio_engine_push(const int16_t *mono, int count); // queue for playback
int  audio_engine_pull(int16_t *mono, int count);        // pull mic samples

// Query
int  audio_engine_tx_space();    // how many mono samples can be pushed
int  audio_engine_rx_avail();    // how many mic samples are ready

// Volume (0..100)
void audio_engine_set_volume(int vol);
int  audio_engine_get_volume();

// Mute/unmute output (e.g. during recording to avoid feedback)
void audio_engine_mute();
void audio_engine_unmute();
