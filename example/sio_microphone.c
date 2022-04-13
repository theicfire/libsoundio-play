/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libsoundio, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <soundio/soundio.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool make_sound = false;
struct SoundIoRingBuffer *ring_buffer = NULL;

static enum SoundIoFormat prioritized_formats[] = {
    SoundIoFormatFloat32NE, SoundIoFormatFloat32FE, SoundIoFormatS32NE,
    SoundIoFormatS32FE,     SoundIoFormatS24NE,     SoundIoFormatS24FE,
    SoundIoFormatS16NE,     SoundIoFormatS16FE,     SoundIoFormatFloat64NE,
    SoundIoFormatFloat64FE, SoundIoFormatU32NE,     SoundIoFormatU32FE,
    SoundIoFormatU24NE,     SoundIoFormatU24FE,     SoundIoFormatU16NE,
    SoundIoFormatU16FE,     SoundIoFormatS8,        SoundIoFormatU8,
    SoundIoFormatInvalid,
};

static double seconds_offset = 0.0;

static int prioritized_sample_rates[] = {
    48000, 44100, 96000, 24000, 0,
};

__attribute__((cold)) __attribute__((noreturn))
__attribute__((format(printf, 1, 2))) static void
panic(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  abort();
}

static int min_int(int a, int b) { return (a < b) ? a : b; }

// static void write_sample_float32ne(char *ptr, double sample) {
//     float *buf = (float *)ptr;
//     // printf("%f ", *buf);
//     *buf = sample;
// }

static void write_sample_s32ne(char *ptr, double sample) {
  int32_t *buf = (int32_t *)ptr;
  double range = (double)INT32_MAX - (double)INT32_MIN;
  double val = sample * range / 2.0;
  *buf = val;
//   printf("WW: %f %0X ", sample, *buf);
}

static void write_sine(struct SoundIoOutStream *outstream, int frame_max) {
  // A frame is 1 or 2 samples. One per channel!

  char *write_ptr = soundio_ring_buffer_write_ptr(ring_buffer);
  int free_bytes = soundio_ring_buffer_free_count(ring_buffer);
  int BYTES_PER_FRAME = outstream->bytes_per_frame;
  const struct SoundIoChannelLayout *layout = &outstream->layout;
  printf("Channels per frame: %d, bytes_per_sample: %d\n",
         layout->channel_count, outstream->bytes_per_sample);
  if (outstream->bytes_per_sample != 4) {
    panic("Bytes per sample is not 4: %d", outstream->bytes_per_sample);
  }
  int free_frames_count = free_bytes / BYTES_PER_FRAME;
  if (free_frames_count < frame_max) {
    panic("No space to write");
  }
    double float_sample_rate = outstream->sample_rate;
    double seconds_per_frame = 1.0 / float_sample_rate;
  int frame_count = frame_max;
    // double seconds_offset = 0.0;

    static const double PI = 3.14159265358979323846264338328;
    double pitch = 440.0;
    double radians_per_second = pitch * 2.0 * PI;

  for (int frame = 0; frame < frame_count; frame += 1) {
    double sample = sin((seconds_offset + frame * seconds_per_frame) *
                          radians_per_second);
    // double sample = (frame % 3) - 1;
    sample *= .1;
    for (int channel = 0; channel < layout->channel_count; channel += 1) {
      write_sample_s32ne(write_ptr, sample);
    }
    write_ptr += BYTES_PER_FRAME;
  }

    seconds_offset =
        fmod(seconds_offset + seconds_per_frame * frame_count, 1.0);
    soundio_ring_buffer_advance_write_ptr(ring_buffer, BYTES_PER_FRAME * frame_count);
}

static void read_callback(struct SoundIoInStream *instream, int frame_count_min,
                          int frame_count_max) {
  return;
  struct SoundIoChannelArea *areas;
  int err;
  char *write_ptr = soundio_ring_buffer_write_ptr(ring_buffer);
  int free_bytes = soundio_ring_buffer_free_count(ring_buffer);
  int free_frames_count = free_bytes / instream->bytes_per_frame;

  if (frame_count_min > free_frames_count)
    panic("ring buffer overflow");

  int write_frames = min_int(free_frames_count, frame_count_max);
  int frames_left = write_frames;

  for (;;) {
    int frame_count = frames_left;

    if ((err = soundio_instream_begin_read(instream, &areas, &frame_count)))
      panic("begin read error: %s", soundio_strerror(err));

    if (!frame_count)
      break;

    if (!areas) {
      // Due to an overflow there is a hole. Fill the ring buffer with
      // silence for the size of the hole.
      memset(write_ptr, 0, frame_count * instream->bytes_per_frame);
      fprintf(stderr, "Dropped %d frames due to internal overflow\n",
              frame_count);
    } else {
      for (int frame = 0; frame < frame_count; frame += 1) {
        for (int ch = 0; ch < instream->layout.channel_count; ch += 1) {
          memcpy(write_ptr, areas[ch].ptr, instream->bytes_per_sample);
          areas[ch].ptr += areas[ch].step;
          write_ptr += instream->bytes_per_sample;
        }
      }
    }

    if ((err = soundio_instream_end_read(instream)))
      panic("end read error: %s", soundio_strerror(err));

    frames_left -= frame_count;
    if (frames_left <= 0)
      break;
  }

  int advance_bytes = write_frames * instream->bytes_per_frame;
  soundio_ring_buffer_advance_write_ptr(ring_buffer, advance_bytes);
}

static inline void write_callback(struct SoundIoOutStream *outstream,
                                  int frame_count_min, int frame_count_max) {
  struct SoundIoChannelArea *areas;
  int frames_left;
  int frame_count;
  int err;

  char *read_ptr = soundio_ring_buffer_read_ptr(ring_buffer);
  int fill_bytes = soundio_ring_buffer_fill_count(ring_buffer);
  int fill_count = fill_bytes / outstream->bytes_per_frame;
  //   printf("Hit write_sine: %d\n", frame_count_max);
  write_sine(outstream, frame_count_max);

  //   printf("write callback: fill_count: %d, frame_count_max: %d\n",
  //   fill_count, frame_count_max);
  if (frame_count_min > fill_count) {
    // Ring buffer does not have enough data, fill with zeroes.
    frames_left = frame_count_min;
    for (;;) {
      frame_count = frames_left;
      if (frame_count <= 0)
        return;
      if ((err =
               soundio_outstream_begin_write(outstream, &areas, &frame_count)))
        panic("begin write error: %s", soundio_strerror(err));
      if (frame_count <= 0)
        return;
      printf("Write %d frames over %d channels\n", frame_count,
             outstream->layout.channel_count);
      for (int frame = 0; frame < frame_count; frame += 1) {
        for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
          memset(areas[ch].ptr, 0, outstream->bytes_per_sample);
          areas[ch].ptr += areas[ch].step;
        }
      }
      if ((err = soundio_outstream_end_write(outstream)))
        panic("end write error: %s", soundio_strerror(err));
      frames_left -= frame_count;
    }
  }

  int read_count = min_int(frame_count_max, fill_count);
  frames_left = read_count;

  while (frames_left > 0) {
    int frame_count = frames_left;

    if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count)))
      panic("begin write error: %s", soundio_strerror(err));

    if (frame_count <= 0)
      break;

    printf("Write %d frames over %d channels\n", frame_count, outstream->layout.channel_count);
    for (int frame = 0; frame < frame_count; frame += 1) {
      for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {

        memcpy(areas[ch].ptr, read_ptr, outstream->bytes_per_sample);
        areas[ch].ptr += areas[ch].step;
        read_ptr += outstream->bytes_per_sample;
      }
    }

    if ((err = soundio_outstream_end_write(outstream)))
      panic("end write error: %s", soundio_strerror(err));

    frames_left -= frame_count;
  }

  soundio_ring_buffer_advance_read_ptr(ring_buffer,
                                       read_count * outstream->bytes_per_frame);
}

// static const double PI = 3.14159265358979323846264338328;
static volatile bool want_pause = false;
inline static void write_callback2(struct SoundIoOutStream *outstream,
                                   int frame_count_min, int frame_count_max) {
  double float_sample_rate = outstream->sample_rate;
  double seconds_per_frame = 1.0 / float_sample_rate;
  struct SoundIoChannelArea *areas;
  int err;

  int frames_left = frame_count_max;

  for (;;) {
    int frame_count = frames_left;
    if ((err =
             soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
      fprintf(stderr, "unrecoverable stream error: %s\n",
              soundio_strerror(err));
      exit(1);
    }

    if (!frame_count)
      break;

    const struct SoundIoChannelLayout *layout = &outstream->layout;
    printf("channels: %d\n", layout->channel_count);

    // double pitch = 440.0;
    // double radians_per_second = pitch * 2.0 * PI;
    for (int frame = 0; frame < frame_count; frame += 1) {
      double sample =
          (frame % 3) - 1; // sin((seconds_offset + frame * seconds_per_frame) *
                           // radians_per_second);
      for (int channel = 0; channel < layout->channel_count; channel += 1) {
        write_sample_s32ne(areas[channel].ptr, sample);
        areas[channel].ptr += areas[channel].step;
      }
    }
    seconds_offset =
        fmod(seconds_offset + seconds_per_frame * frame_count, 1.0);

    if ((err = soundio_outstream_end_write(outstream))) {
      if (err == SoundIoErrorUnderflow)
        return;
      fprintf(stderr, "unrecoverable stream error: %s\n",
              soundio_strerror(err));
      exit(1);
    }

    frames_left -= frame_count;
    if (frames_left <= 0)
      break;
  }

  soundio_outstream_pause(outstream, want_pause);
}

static void underflow_callback(struct SoundIoOutStream *outstream) {
  static int count = 0;
  fprintf(stderr, "underflow %d\n", ++count);
}

static int usage(char *exe) {
  fprintf(stderr,
          "Usage: %s [options]\n"
          "Options:\n"
          "  [--backend dummy|alsa|pulseaudio|jack|coreaudio|wasapi]\n"
          "  [--in-device id]\n"
          "  [--in-raw]\n"
          "  [--out-device id]\n"
          "  [--out-raw]\n"
          "  [--latency seconds]\n",
          exe);
  return 1;
}

int main(int argc, char **argv) {
  char *exe = argv[0];
  enum SoundIoBackend backend = SoundIoBackendNone;
  char *in_device_id = NULL;
  char *out_device_id = NULL;
  bool in_raw = false;
  bool out_raw = false;

  double microphone_latency = 0.2; // seconds

  for (int i = 1; i < argc; i += 1) {
    char *arg = argv[i];
    if (arg[0] == '-' && arg[1] == '-') {
      if (strcmp(arg, "--in-raw") == 0) {
        in_raw = true;
      } else if (strcmp(arg, "--out-raw") == 0) {
        out_raw = true;
      } else if (++i >= argc) {
        return usage(exe);
      } else if (strcmp(arg, "--backend") == 0) {
        if (strcmp("dummy", argv[i]) == 0) {
          backend = SoundIoBackendDummy;
        } else if (strcmp("alsa", argv[i]) == 0) {
          backend = SoundIoBackendAlsa;
        } else if (strcmp("pulseaudio", argv[i]) == 0) {
          backend = SoundIoBackendPulseAudio;
        } else if (strcmp("jack", argv[i]) == 0) {
          backend = SoundIoBackendJack;
        } else if (strcmp("coreaudio", argv[i]) == 0) {
          backend = SoundIoBackendCoreAudio;
        } else if (strcmp("wasapi", argv[i]) == 0) {
          backend = SoundIoBackendWasapi;
        } else {
          fprintf(stderr, "Invalid backend: %s\n", argv[i]);
          return 1;
        }
      } else if (strcmp(arg, "--in-device") == 0) {
        in_device_id = argv[i];
      } else if (strcmp(arg, "--out-device") == 0) {
        out_device_id = argv[i];
      } else if (strcmp(arg, "--latency") == 0) {
        microphone_latency = atof(argv[i]);
      } else {
        return usage(exe);
      }
    } else {
      return usage(exe);
    }
  }
  struct SoundIo *soundio = soundio_create();
  if (!soundio)
    panic("out of memory");

  int err = (backend == SoundIoBackendNone)
                ? soundio_connect(soundio)
                : soundio_connect_backend(soundio, backend);
  if (err)
    panic("error connecting: %s", soundio_strerror(err));

  soundio_flush_events(soundio);

  int default_out_device_index = soundio_default_output_device_index(soundio);
  if (default_out_device_index < 0)
    panic("no output device found");

  int default_in_device_index = soundio_default_input_device_index(soundio);
  if (default_in_device_index < 0)
    panic("no input device found");

  int in_device_index = default_in_device_index;
  if (in_device_id) {
    bool found = false;
    for (int i = 0; i < soundio_input_device_count(soundio); i += 1) {
      struct SoundIoDevice *device = soundio_get_input_device(soundio, i);
      if (device->is_raw == in_raw && strcmp(device->id, in_device_id) == 0) {
        in_device_index = i;
        found = true;
        soundio_device_unref(device);
        break;
      }
      soundio_device_unref(device);
    }
    if (!found)
      panic("invalid input device id: %s", in_device_id);
  }

  int out_device_index = default_out_device_index;
  if (out_device_id) {
    bool found = false;
    for (int i = 0; i < soundio_output_device_count(soundio); i += 1) {
      struct SoundIoDevice *device = soundio_get_output_device(soundio, i);
      if (device->is_raw == out_raw && strcmp(device->id, out_device_id) == 0) {
        out_device_index = i;
        found = true;
        soundio_device_unref(device);
        break;
      }
      soundio_device_unref(device);
    }
    if (!found)
      panic("invalid output device id: %s", out_device_id);
  }

  struct SoundIoDevice *out_device =
      soundio_get_output_device(soundio, out_device_index);
  if (!out_device)
    panic("could not get output device: out of memory");

  struct SoundIoDevice *in_device =
      soundio_get_input_device(soundio, in_device_index);
  if (!in_device)
    panic("could not get input device: out of memory");

  fprintf(stderr, "Input device: %s\n", in_device->name);
  fprintf(stderr, "Output device: %s\n", out_device->name);

  //   soundio_device_sort_channel_layouts(out_device);
  //   for (int i = 0; i < out_device->layout_count; i++) {
  //       printf("LISTING: ch: %d || name: %s\n",
  //       out_device->layouts[i].channel_count, out_device->layouts[i].name);
  //   }
  const struct SoundIoChannelLayout *layout =
      soundio_best_matching_channel_layout(
          out_device->layouts, out_device->layout_count, in_device->layouts,
          in_device->layout_count);

  if (!layout)
    panic("channel layouts not compatible");

  int *sample_rate;
  for (sample_rate = prioritized_sample_rates; *sample_rate; sample_rate += 1) {
    if (soundio_device_supports_sample_rate(in_device, *sample_rate) &&
        soundio_device_supports_sample_rate(out_device, *sample_rate)) {
      break;
    }
  }
  if (!*sample_rate)
    panic("incompatible sample rates");

  enum SoundIoFormat *fmt;
  for (fmt = prioritized_formats; *fmt != SoundIoFormatInvalid; fmt += 1) {
    if (soundio_device_supports_format(in_device, *fmt) &&
        soundio_device_supports_format(out_device, *fmt)) {
      break;
    }
  }
  if (*fmt == SoundIoFormatInvalid)
    panic("incompatible sample formats");

  struct SoundIoInStream *instream = soundio_instream_create(in_device);
  if (!instream)
    panic("out of memory");
  //   instream->format = *fmt;
  instream->format = SoundIoFormatS32NE;
  printf("instream format: %d\n", *fmt);
  instream->sample_rate = *sample_rate;
  instream->layout = *layout;
  instream->software_latency = microphone_latency;
  instream->read_callback = read_callback;

  if ((err = soundio_instream_open(instream))) {
    fprintf(stderr, "unable to open input stream: %s", soundio_strerror(err));
    return 1;
  }

  struct SoundIoOutStream *outstream = soundio_outstream_create(out_device);
  if (!outstream)
    panic("out of memory");
  //   outstream->format = *fmt;
  outstream->format = SoundIoFormatS32NE;
  printf("outstream format: %d\n", *fmt);
  outstream->sample_rate = *sample_rate;
  outstream->layout = *layout;
  outstream->software_latency = microphone_latency;
  outstream->write_callback = write_callback;
  outstream->underflow_callback = underflow_callback;

  if ((err = soundio_outstream_open(outstream))) {
    fprintf(stderr, "unable to open output stream: %s", soundio_strerror(err));
    return 1;
  }

  int capacity = microphone_latency * 2 * instream->sample_rate *
                 instream->bytes_per_frame;
  ring_buffer = soundio_ring_buffer_create(soundio, capacity);
  if (!ring_buffer)
    panic("unable to create ring buffer: out of memory");
  char *buf = soundio_ring_buffer_write_ptr(ring_buffer);
  int fill_count =
      microphone_latency * outstream->sample_rate * outstream->bytes_per_frame;
  memset(buf, 0, fill_count);
  soundio_ring_buffer_advance_write_ptr(ring_buffer, fill_count);

  if ((err = soundio_instream_start(instream)))
    panic("unable to start input device: %s", soundio_strerror(err));

  if ((err = soundio_outstream_start(outstream)))
    panic("unable to start output device: %s", soundio_strerror(err));

  //   for (;;)
  //     soundio_wait_events(soundio);

  for (;;) {
    soundio_flush_events(soundio);
    int c = getc(stdin);
    if (c == 'w') {
      printf("Writing sine\n");
    //   write_sine(outstream, 14400);
    } else if (c == 'm') {
      make_sound = !make_sound;
    } else if (c == '\r' || c == '\n') {
      // ignore
    } else {
      fprintf(stderr, "Unrecognized command: %c\n", c);
    }
  }

  soundio_outstream_destroy(outstream);
  soundio_instream_destroy(instream);
  soundio_device_unref(in_device);
  soundio_device_unref(out_device);
  soundio_destroy(soundio);
  return 0;
}
