/* Host-test stand-in for <aaudio/AAudio.h>: audio_out_test runs with TOTK_AUDIO_OFF, so no stream is ever opened. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
typedef int32_t aaudio_result_t;
typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
enum { AAUDIO_OK = 0, AAUDIO_FORMAT_PCM_I16 = 1, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12, AAUDIO_USAGE_GAME = 14 };
static inline aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **b) { (void)b; abort(); }
static inline void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, int f) { (void)b; (void)f; abort(); }
static inline void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int n) { (void)b; (void)n; abort(); }
static inline void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t r) { (void)b; (void)r; abort(); }
static inline void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, int m) { (void)b; (void)m; abort(); }
static inline void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, int u) { (void)b; (void)u; abort(); }
static inline aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **s) { (void)b; (void)s; abort(); }
static inline aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { (void)b; abort(); }
static inline int32_t AAudioStream_getSampleRate(AAudioStream *s) { (void)s; abort(); }
static inline aaudio_result_t AAudioStream_requestStart(AAudioStream *s) { (void)s; abort(); }
static inline aaudio_result_t AAudioStream_requestStop(AAudioStream *s) { (void)s; abort(); }
static inline aaudio_result_t AAudioStream_close(AAudioStream *s) { (void)s; abort(); }
static inline int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { (void)s; abort(); }
static inline aaudio_result_t AAudioStream_write(AAudioStream *s, const void *d, int32_t n, int64_t t) { (void)s; (void)d; (void)n; (void)t; abort(); }
static inline const char *AAudio_convertResultToText(aaudio_result_t r) { (void)r; return "test"; }
