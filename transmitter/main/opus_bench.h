#pragma once

/* Timing and memory benchmarks. Each spawns a task, reports, then exits.
 * Diagnostic only - none of these is part of the audio path. */

/* Opus: sweeps sample rate, complexity and frame size for a single stream. */
void opus_bench_run(void);

/* Opus: builds the real multi-stream workloads and times each as a whole. */
void opus_load_run(void);

/* AMR-NB: all eight bitrate modes, then the multi-stream workloads. */
void amr_bench_run(void);

/* All three codecs measured on the same axes: CPU, memory, stack, bytes on the
 * wire, and algorithmic delay. */
void codec_bench_run(void);

/* Specific stream mixes, measured on one core and split across two. */
void opus_scenarios_run(void);
