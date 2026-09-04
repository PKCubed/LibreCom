#pragma once

/* Spawns a task that benchmarks Opus encode/decode across sample rates and
 * complexity settings, then deletes itself. Diagnostic only. */
void opus_bench_run(void);
