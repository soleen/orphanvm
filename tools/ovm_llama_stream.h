/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OVM_LLAMA_STREAM_H
#define OVM_LLAMA_STREAM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVM_LLAMA_MAGIC 0x4f564d5f4c4c4d41ULL /* "OVM_LLMA" */
#define OVM_LLAMA_VERSION 1

#define OVM_LLAMA_STATUS_INIT       0
#define OVM_LLAMA_STATUS_LOADING    1
#define OVM_LLAMA_STATUS_GENERATING 2
#define OVM_LLAMA_STATUS_FINISHED   3
#define OVM_LLAMA_STATUS_ERROR      4

#define OVM_LLAMA_BUF_SIZE (1024 * 1024) /* 1MB text ring buffer */

struct ovm_llama_stream {
	uint64_t magic;          /* OVM_LLAMA_MAGIC */
	uint32_t version;        /* OVM_LLAMA_VERSION */
	uint32_t status;         /* OVM_LLAMA_STATUS_* */
	uint64_t gpa;            /* Guest Physical Address of this struct */
	uint64_t total_tokens;   /* Total tokens generated */
	uint64_t total_chars;    /* Total characters written */
	uint64_t head;           /* Write pointer in buffer (0 .. OVM_LLAMA_BUF_SIZE - 1) */
	uint64_t prompt_tokens;  /* Number of prompt tokens processed */
	uint64_t start_time_ms;  /* Monotonic start time ms */
	uint64_t last_time_ms;   /* Timestamp of last generated token ms */
	uint64_t reserved[6];
	char buffer[OVM_LLAMA_BUF_SIZE];
};

void ovm_llama_stream_init(void);
void ovm_llama_stream_set_status(uint32_t status);
void ovm_llama_stream_set_prompt_tokens(uint64_t count);
void ovm_llama_stream_append(const char *piece, size_t len);
void ovm_llama_stream_finish(void);
struct ovm_llama_stream *ovm_llama_stream_get(void);

#ifdef __cplusplus
}
#endif

#endif /* OVM_LLAMA_STREAM_H */
