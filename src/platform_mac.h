#ifndef PLATFORM_MAC_H
#define PLATFORM_MAC_H

#ifdef __APPLE__

#include <AudioToolbox/AudioQueue.h>
#include <pthread.h>

#define NUM_BUFFERS 3

extern bool g_continuous_playback;

typedef struct {
  AudioQueueRef queue;
  AudioQueueBufferRef buffers[NUM_BUFFERS];
  int sample_rate;
  int buffer_samples;
  bool playing;
  bool terminated;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  
  // For visualizer: shared audio buffer
  short *shared_buffer;
  int shared_buffer_len;
  pthread_mutex_t buffer_mutex;
} AudioContext;

bool initAudioQueue(AudioContext *ctx, int sample_rate, int buffer_samples);
void cleanupAudioQueue(AudioContext *ctx);

#endif // __APPLE__

#endif // PLATFORM_MAC_H
