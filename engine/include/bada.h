/* bada: a bada-compatible v2.0 engine (docs/bada-compatible-v2.md).
 * Written by bada-compat's export!: don't edit it. */
#ifndef BADA_H
#define BADA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bada_data bada_data;
typedef struct bada_session bada_session;
typedef struct bada_engine bada_engine;

/* One named buffer of bada_data_new. */
typedef struct { const char *name; const uint8_t *ptr; size_t len; uint32_t flags; } bada_buf;
/* The buffer outlives the data: the engine may keep pointers into it. */
#define BADA_BORROWED 1u

/* Static JSON: what the engine is and has. No engine needed. */
const char *bada_describe(void);

/* Immutable, shared, thread-safe. NULL on failure. */
const bada_data *bada_data_new(const bada_buf *bufs, size_t n);
void bada_data_free(const bada_data *data);

/* Settings, layouts and the user dictionary of one process. NULL if init_json has no harness. */
bada_session *bada_session_new(const bada_data *data, const char *init_json);
void bada_session_free(bada_session *session);

/* One text field. */
bada_engine *bada_new(bada_session *session);
void bada_free(bada_engine *engine);

/* One event in, one reply out. The reply is valid until the next call on the same engine, or on
 * the session when engine is NULL. */
const char *bada_send(bada_session *session, bada_engine *engine_or_null, const char *event_json);

#ifdef __cplusplus
}
#endif

#endif
