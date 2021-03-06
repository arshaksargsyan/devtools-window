/*
 * Copyright © 2013 Mozilla Foundation
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#include <stddef.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"

#define NELEMS(x) ((int) (sizeof(x) / sizeof(x[0])))

struct cubeb {
  struct cubeb_ops * ops;
};

struct cubeb_stream {
  struct cubeb * context;
};

#ifdef USE_PULSE
int pulse_init(cubeb ** context, char const * context_name);
#endif
#ifdef USE_ALSA
int alsa_init(cubeb ** context, char const * context_name);
#endif
#ifdef USE_AUDIOQUEUE
int audioqueue_init(cubeb ** context, char const * context_name);
#endif
#ifdef USE_AUDIOUNIT
int audiounit_init(cubeb ** context, char const * context_name);
#endif
#ifdef USE_DIRECTSOUND
int directsound_init(cubeb ** context, char const * context_name);
#endif
#ifdef USE_WINMM
int winmm_init(cubeb ** context, char const * context_name);
#endif
#ifdef USE_SNDIO
int sndio_init(cubeb ** context, char const * context_name);
#endif
#ifdef USE_OPENSL
int opensl_init(cubeb ** context, char const * context_name);
#endif

int
validate_stream_params(cubeb_stream_params stream_params)
{
  if (stream_params.rate < 1 || stream_params.rate > 192000 ||
      stream_params.channels < 1 || stream_params.channels > 8) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  switch (stream_params.format) {
  case CUBEB_SAMPLE_S16LE:
  case CUBEB_SAMPLE_S16BE:
  case CUBEB_SAMPLE_FLOAT32LE:
  case CUBEB_SAMPLE_FLOAT32BE:
    return CUBEB_OK;
  }

  return CUBEB_ERROR_INVALID_FORMAT;
}

int
validate_latency(int latency)
{
  if (latency < 1 || latency > 2000) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }
  return CUBEB_OK;
}

int
cubeb_init(cubeb ** context, char const * context_name)
{
  int (* init[])(cubeb **, char const *) = {
#ifdef USE_PULSE
    pulse_init,
#endif
#ifdef USE_ALSA
    alsa_init,
#endif
#ifdef USE_AUDIOUNIT
    audiounit_init,
#endif
#ifdef USE_AUDIOQUEUE
    audioqueue_init,
#endif
#ifdef USE_WINMM
    winmm_init,
#endif
#ifdef USE_DIRECTSOUND
    directsound_init,
#endif
#ifdef USE_SNDIO
    sndio_init,
#endif
#ifdef USE_OPENSL
    opensl_init,
#endif
  };
  int i;

  if (!context) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  for (i = 0; i < NELEMS(init); ++i) {
    if (init[i](context, context_name) == CUBEB_OK) {
      return CUBEB_OK;
    }
  }

  return CUBEB_ERROR;
}

char const *
cubeb_get_backend_id(cubeb * context)
{
  if (!context) {
    return NULL;
  }

  return context->ops->get_backend_id(context);
}

void
cubeb_destroy(cubeb * context)
{
  if (!context) {
    return;
  }

  context->ops->destroy(context);
}

int
cubeb_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                  cubeb_stream_params stream_params, unsigned int latency,
                  cubeb_data_callback data_callback,
                  cubeb_state_callback state_callback,
                  void * user_ptr)
{
  int r;

  if (!context || !stream) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  if ((r = validate_stream_params(stream_params)) != CUBEB_OK ||
      (r = validate_latency(latency)) != CUBEB_OK) {
    return r;
  }

  return context->ops->stream_init(context, stream, stream_name,
                                   stream_params, latency,
                                   data_callback,
                                   state_callback,
                                   user_ptr);
}

void
cubeb_stream_destroy(cubeb_stream * stream)
{
  if (!stream) {
    return;
  }

  stream->context->ops->stream_destroy(stream);
}

int
cubeb_stream_start(cubeb_stream * stream)
{
  if (!stream) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  return stream->context->ops->stream_start(stream);
}

int
cubeb_stream_stop(cubeb_stream * stream)
{
  if (!stream) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  return stream->context->ops->stream_stop(stream);
}

int
cubeb_stream_get_position(cubeb_stream * stream, uint64_t * position)
{
  if (!stream || !position) {
    return CUBEB_ERROR_INVALID_PARAMETER;
  }

  return stream->context->ops->stream_get_position(stream, position);
}
