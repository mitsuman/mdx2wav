#ifdef __APPLE__

#include "platform_mac.h"
#include <stdio.h>
#include <string.h>
#include <AudioToolbox/AudioQueue.h>
#include <pthread.h>
#include "../gamdx/mxdrvg/mxdrvg.h"

static AudioPCMFillFunc g_audio_fill = nullptr;

void setAudioPCMFillFunc(AudioPCMFillFunc func) {
  g_audio_fill = func;
}

static void audioCallback(void *userData, AudioQueueRef queue, AudioQueueBufferRef buffer) {
  AudioContext *ctx = (AudioContext *)userData;
  
  // continuous playbackモードでは曲終了を無視
  if (!ctx->playing) {
    ctx->terminated = true;
    pthread_cond_signal(&ctx->cond);
    return;
  }

  short *audio_buf = (short *)buffer->mAudioData;
  int len;
  if (g_audio_fill) {
    // SPC (and any future non-MXDRVG source): the callback owns generation.
    len = g_audio_fill(audio_buf, ctx->buffer_samples);
  } else {
    // continuous playbackモードでは曲終了を無視
    if (!g_continuous_playback && (ctx->terminated || MXDRVG_GetTerminated())) {
      ctx->terminated = true;
      pthread_cond_signal(&ctx->cond);
      return;
    }
    len = MXDRVG_GetPCM(audio_buf, ctx->buffer_samples);
  }
  
  if (len <= 0) {
    if (!g_continuous_playback) {
      ctx->terminated = true;
      pthread_cond_signal(&ctx->cond);
      return;
    }
    // Visualizer playback stays alive after the song ends: feed silence so the
    // queue keeps running and a restart or file switch can fill it again.
    memset(audio_buf, 0, (size_t)ctx->buffer_samples * 4);
    len = ctx->buffer_samples;
  }
  
  // Copy to shared buffer for visualizer
  pthread_mutex_lock(&ctx->buffer_mutex);
  if (ctx->shared_buffer) {
    memcpy(ctx->shared_buffer, audio_buf, len * 4); // 2 channels * 2 bytes per sample
    ctx->shared_buffer_len = len;
  }
  pthread_mutex_unlock(&ctx->buffer_mutex);
  
  buffer->mAudioDataByteSize = len * 4; // 2 channels * 2 bytes
  AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

bool initAudioQueue(AudioContext *ctx, int sample_rate, int buffer_samples) {
  ctx->sample_rate = sample_rate;
  ctx->buffer_samples = buffer_samples;
  ctx->playing = true;
  ctx->terminated = false;
  pthread_mutex_init(&ctx->mutex, NULL);
  pthread_cond_init(&ctx->cond, NULL);
  
  // Initialize shared buffer for visualizer
  ctx->shared_buffer = new short[buffer_samples * 2];
  ctx->shared_buffer_len = 0;
  pthread_mutex_init(&ctx->buffer_mutex, NULL);
  
  AudioStreamBasicDescription format = {0};
  format.mSampleRate = sample_rate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  format.mBitsPerChannel = 16;
  format.mChannelsPerFrame = 2;
  format.mBytesPerFrame = 4;
  format.mFramesPerPacket = 1;
  format.mBytesPerPacket = 4;
  
  OSStatus status = AudioQueueNewOutput(&format, audioCallback, ctx, NULL, NULL, 0, &ctx->queue);
  if (status != noErr) {
    fprintf(stderr, "Failed to create audio queue: %d\n", status);
    return false;
  }
  
  for (int i = 0; i < NUM_BUFFERS; i++) {
    status = AudioQueueAllocateBuffer(ctx->queue, buffer_samples * 4, &ctx->buffers[i]);
    if (status != noErr) {
      fprintf(stderr, "Failed to allocate audio buffer: %d\n", status);
      return false;
    }
    
    // Fill initial buffers
    audioCallback(ctx, ctx->queue, ctx->buffers[i]);
  }
  
  status = AudioQueueStart(ctx->queue, NULL);
  if (status != noErr) {
    fprintf(stderr, "Failed to start audio queue: %d\n", status);
    return false;
  }
  
  return true;
}

void cleanupAudioQueue(AudioContext *ctx) {
  if (ctx->queue) {
    AudioQueueStop(ctx->queue, true);
    AudioQueueDispose(ctx->queue, true);
    ctx->queue = NULL;
  }
  pthread_mutex_destroy(&ctx->mutex);
  pthread_cond_destroy(&ctx->cond);
  
  if (ctx->shared_buffer) {
    delete[] ctx->shared_buffer;
    ctx->shared_buffer = NULL;
  }
  pthread_mutex_destroy(&ctx->buffer_mutex);
}

#endif // __APPLE__
