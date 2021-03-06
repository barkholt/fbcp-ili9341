#include <bcm_host.h>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <stdio.h>

#include "config.h"
#include "gpu.h"
#include "display.h"
#include "tick.h"
#include "util.h"
#include "statistics.h"

DISPMANX_DISPLAY_HANDLE_T display;
DISPMANX_RESOURCE_HANDLE_T screen_resource;
VC_RECT_T rect;

int frameTimeHistorySize = 0;

FrameHistory frameTimeHistory[FRAME_HISTORY_MAX_SIZE] = {};

uint16_t *videoCoreFramebuffer[2] = {};
volatile int numNewGpuFrames = 0;

int displayXOffset = 0;
int displayYOffset = 0;

#ifdef USE_GPU_VSYNC

volatile /*bool*/uint32_t gpuFrameAvailable = 0;

void VsyncCallback(DISPMANX_UPDATE_HANDLE_T u, void *arg)
{
  __atomic_store_n(&gpuFrameAvailable, 1, __ATOMIC_SEQ_CST);
  syscall(SYS_futex, &gpuFrameAvailable, FUTEX_WAKE, 1, 0, 0, 0); // Wake the main thread to process a new frame
}

uint64_t EstimateFrameRateInterval()
{
  return 1000000/60;
}

#else // !USE_GPU_VSYNC

// Since we are polling for received GPU frames, run a histogram to predict when the next frame will arrive.
// The histogram needs to be sufficiently small as to not cause a lag when frame rate suddenly changes on e.g.
// main menu <-> ingame transitions
#define HISTOGRAM_SIZE 30
uint64_t frameArrivalTimes[HISTOGRAM_SIZE];
uint64_t frameArrivalTimesTail = 0;
uint64_t lastFramePollTime = 0;
int histogramSize = 0;

// Returns Nth most recent entry in the frame times histogram, 0 = most recent, (histogramSize-1) = oldest
#define GET_HISTOGRAM(idx) frameArrivalTimes[(frameArrivalTimesTail - 1 - (idx) + HISTOGRAM_SIZE) % HISTOGRAM_SIZE]

void AddHistogramSample()
{
  frameArrivalTimes[frameArrivalTimesTail] = tick();
  frameArrivalTimesTail = (frameArrivalTimesTail + 1) % HISTOGRAM_SIZE;
  if (histogramSize < HISTOGRAM_SIZE) ++histogramSize;
}

int cmp(const void *e1, const void *e2) { return *(uint64_t*)e1 > *(uint64_t*)e2; }

uint64_t EstimateFrameRateInterval()
{
  if (histogramSize == 0) return 1000000/TARGET_FRAME_RATE;
  uint64_t mostRecentFrame = GET_HISTOGRAM(0);

  // High sleep mode hacks to save battery when ~idle: (These could be removed with an event based VideoCore display refresh API)
  uint64_t timeNow = tick();
#ifdef SAVE_BATTERY_BY_SLEEPING_WHEN_IDLE
  if (timeNow - mostRecentFrame > 60000000) { histogramSize = 1; return 500000; } // if it's been more than one minute since last seen update, assume interval of 500ms.
  if (timeNow - mostRecentFrame > 100000) return 100000; // if it's been more than 100ms since last seen update, assume interval of 100ms.
  if (histogramSize <= HISTOGRAM_SIZE) return 1000000/TARGET_FRAME_RATE;
#ifndef SAVE_BATTERY_BY_PREDICTING_FRAME_ARRIVAL_TIMES
  return 1000000/TARGET_FRAME_RATE;
#endif
#endif

  // Look at the intervals of all previous arrived frames, and take their 40% percentile as our expected current frame rate
  uint64_t intervals[HISTOGRAM_SIZE-1];
  for(int i = 0; i < histogramSize-1; ++i)
    intervals[i] = GET_HISTOGRAM(i) - GET_HISTOGRAM(i+1);
  qsort(intervals, histogramSize-1, sizeof(uint64_t), cmp);
  uint64_t interval = intervals[(histogramSize-1)*2/5];

  // With bad luck, we may actually have synchronized to observing every second update, so halve the computed interval if it looks like a long period of time
  if (interval >= 2000000/TARGET_FRAME_RATE) interval /= 2;
  if (interval > 100000) interval = 100000;
  return MAX(interval, 1000000/TARGET_FRAME_RATE);

}

uint64_t PredictNextFrameArrivalTime()
{
  uint64_t mostRecentFrame = histogramSize > 0 ? GET_HISTOGRAM(0) : tick();

  // High sleep mode hacks to save battery when ~idle: (These could be removed with an event based VideoCore display refresh API)
  uint64_t timeNow = tick();
#ifdef SAVE_BATTERY_BY_SLEEPING_WHEN_IDLE
  if (timeNow - mostRecentFrame > 60000000) { histogramSize = 1; return lastFramePollTime + 100000; } // if it's been more than one minute since last seen update, assume interval of 500ms.
  if (timeNow - mostRecentFrame > 100000) return lastFramePollTime + 100000; // if it's been more than 100ms since last seen update, assume interval of 100ms.
#endif
  uint64_t interval = EstimateFrameRateInterval();

  // Assume that frames are arriving at times mostRecentFrame + k * interval.
  // Find integer k such that mostRecentFrame + k * interval >= timeNow
  // i.e. k = ceil((timeNow - mostRecentFrame) / interval)
  uint64_t k = (timeNow - mostRecentFrame + interval - 1) / interval;
  uint64_t nextFrameArrivalTime = mostRecentFrame + k * interval;
  uint64_t timeOfPreviousMissedFrame = nextFrameArrivalTime - interval;

  // If there should have been a frame just 1/3rd of our interval window ago, assume it was just missed and report back "the next frame is right now"
  if (timeNow - timeOfPreviousMissedFrame < interval/3 && timeOfPreviousMissedFrame > mostRecentFrame) return timeNow;
  else return nextFrameArrivalTime;
}

#endif // ~USE_GPU_VSYNC

void *gpu_polling_thread(void*)
{
  uint64_t lastNewFrameReceivedTime = tick();
  for(;;)
  {
#ifdef SAVE_BATTERY_BY_SLEEPING_UNTIL_TARGET_FRAME
    const int64_t earlyFramePrediction = 500;
    uint64_t earliestNextFrameArrivaltime = lastNewFrameReceivedTime + 1000000/TARGET_FRAME_RATE - earlyFramePrediction;
    uint64_t now = tick();
    if (now < earliestNextFrameArrivaltime)
    {
      usleep(earliestNextFrameArrivaltime - now);
    }
#endif

#if defined(SAVE_BATTERY_BY_PREDICTING_FRAME_ARRIVAL_TIMES) || defined(SAVE_BATTERY_BY_SLEEPING_WHEN_IDLE)
    uint64_t nextFrameArrivalTime = PredictNextFrameArrivalTime();
    int64_t timeToSleep = nextFrameArrivalTime - tick();
    const int64_t minimumSleepTime = 2500; // Don't sleep if the next frame is expected to arrive in less than this much time
    if (timeToSleep > minimumSleepTime)
    {
      usleep(timeToSleep - minimumSleepTime);
    }
#endif

   uint64_t t0 = tick();
    // Grab a new frame from the GPU. TODO: Figure out a way to get a frame callback for each GPU-rendered frame,
    // that would be vastly superior for lower latency, reduced stuttering and lighter processing overhead.
    // Currently this implemented method just takes a snapshot of the most current GPU framebuffer contents,
    // without any concept of "finished frames". If this is the case, it's possible that this could grab the same
    // frame twice, and then potentially missing, or displaying the later appearing new frame at a very last moment.
    // Profiling, the following two lines take around ~1msec of time.
    vc_dispmanx_snapshot(display, screen_resource, (DISPMANX_TRANSFORM_T)0);
    vc_dispmanx_resource_read_data(screen_resource, &rect, videoCoreFramebuffer[0], SCANLINE_SIZE);
#ifndef USE_GPU_VSYNC
    lastFramePollTime = t0;
#endif

    // Check the pixel contents of the snapshot to see if we actually received a new frame to render
    bool gotNewFramebuffer = false;
    for(uint32_t *newfb = (uint32_t*)videoCoreFramebuffer[0], *oldfb = (uint32_t*)videoCoreFramebuffer[1], *endfb = (uint32_t*)videoCoreFramebuffer[1] + FRAMEBUFFER_SIZE/4; oldfb < endfb;)
      if (*newfb++ != *oldfb++)
      {
        gotNewFramebuffer = true;
        lastNewFrameReceivedTime = t0;
        break;
      }

    uint64_t t1 = tick();
    if (!gotNewFramebuffer)
    {
#ifdef STATISTICS
      __atomic_fetch_add(&timeWastedPollingGPU, t1-t0, __ATOMIC_RELAXED);
#endif
      continue;
    }
    else
    {
      memcpy(videoCoreFramebuffer[1], videoCoreFramebuffer[0], FRAMEBUFFER_SIZE);
      __atomic_fetch_add(&numNewGpuFrames, 1, __ATOMIC_SEQ_CST);
      syscall(SYS_futex, &numNewGpuFrames, FUTEX_WAKE, 1, 0, 0, 0); // Wake the main thread if it was sleeping to get a new frame
    }
  }
}

void InitGPU()
{
  videoCoreFramebuffer[0] = (uint16_t *)malloc(FRAMEBUFFER_SIZE);
  videoCoreFramebuffer[1] = (uint16_t *)malloc(FRAMEBUFFER_SIZE);
  memset(videoCoreFramebuffer[0], 0, FRAMEBUFFER_SIZE);
  memset(videoCoreFramebuffer[1], 0, FRAMEBUFFER_SIZE);

  // Initialize GPU frame grabbing subsystem
  bcm_host_init();
  display = vc_dispmanx_display_open(0);
  if (!display) FATAL_ERROR("vc_dispmanx_display_open failed!");
  DISPMANX_MODEINFO_T display_info;
  int ret = vc_dispmanx_display_get_info(display, &display_info);
  if (ret) FATAL_ERROR("vc_dispmanx_display_get_info failed!");
  // We may need to scale the main framebuffer to fit the native pixel size of the display. Always want to do such scaling in aspect ratio fixed mode to not stretch the image.
  // (For non-square pixels or similar, could apply a correction factor here to fix aspect ratio)
  displayXOffset = 0;
  displayYOffset = 0;
  int scaledWidth = DISPLAY_WIDTH;
  int scaledHeight = DISPLAY_HEIGHT;
  double scalingFactor = 1.0;

  if (DISPLAY_WIDTH * display_info.height < DISPLAY_HEIGHT * display_info.width)
  {
    scaledHeight = (int)((double)DISPLAY_WIDTH * display_info.height / display_info.width + 0.5);
    scalingFactor = (double)DISPLAY_WIDTH/display_info.width;
    displayYOffset = (DISPLAY_HEIGHT - scaledHeight) / 2;
  }
  else
  {
    scaledWidth = (int)((double)DISPLAY_HEIGHT * display_info.width / display_info.height + 0.5);
    scalingFactor = (double)DISPLAY_HEIGHT/display_info.height;
    displayXOffset = (DISPLAY_WIDTH - scaledWidth) / 2;
  }

  syslog(LOG_INFO, "GPU display is %dx%d. SPI display is %dx%d. Applying scaling factor %.2fx, xOffset: %d, yOffset: %d, scaledWidth: %d, scaledHeight: %d", display_info.width, display_info.height, DISPLAY_WIDTH, DISPLAY_HEIGHT, scalingFactor, displayXOffset, displayYOffset, scaledWidth, scaledHeight);
  printf("GPU display is %dx%d. SPI display is %dx%d. Applying scaling factor %.2fx, xOffset: %d, yOffset: %d, scaledWidth: %d, scaledHeight: %d\n", display_info.width, display_info.height, DISPLAY_WIDTH, DISPLAY_HEIGHT, scalingFactor, displayXOffset, displayYOffset, scaledWidth, scaledHeight);

  uint32_t image_prt;
  screen_resource = vc_dispmanx_resource_create(VC_IMAGE_RGB565, scaledWidth, scaledHeight, &image_prt);
  if (!screen_resource) FATAL_ERROR("vc_dispmanx_resource_create failed!");
  vc_dispmanx_rect_set(&rect, 0, 0, scaledWidth, scaledHeight);

  pthread_t gpuPollingThread;
  int rc = pthread_create(&gpuPollingThread, NULL, gpu_polling_thread, NULL); // After creating the thread, it is assumed to have ownership of the SPI bus, so no SPI chat on the main thread after this.
  if (rc != 0) FATAL_ERROR("Failed to create GPU polling thread!");

#ifdef USE_GPU_VSYNC
  // Register to receive vsync notifications. This is a heuristic, since the application might not be locked at vsync, and even
  // if it was, this signal is not a guaranteed edge trigger for availability of new frames.
  vc_dispmanx_vsync_callback(display, VsyncCallback, 0);
#endif
}
