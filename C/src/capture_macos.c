#include "capture.h"
#include "platform.h"

#ifdef PLATFORM_MACOS

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_BUFS 2

struct webcam_impl {
  // AVFoundation objects
  AVCaptureSession *session;
  AVCaptureDeviceInput *input;
  AVCaptureVideoDataOutput *output;
  id<AVCaptureVideoDataOutputSampleBufferDelegate> delegate;
  dispatch_queue_t queue;

  // Frame ring buffer
  uint8_t *gray_buf[FRAME_BUFS];
  int buf_w, buf_h;
  int write_idx;
  int ready_idx;
  int has_frame;

  pthread_mutex_t lock;
  pthread_cond_t cond;
  int stopped;
};

@interface FrameDelegate
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(assign) struct webcam_impl *impl;
@end

@implementation FrameDelegate

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection *)connection {

  struct webcam_impl *im = self.impl;
  if (!im || im->stopped)
    return;

  CVPixelBufferRef pixbuf = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!pixbuf)
    return;

  CVPixelBufferLockBaseAddress(pixbuf, kCVPixelBufferLock_ReadOnly);

  // Request NV12 (YUV 4:2:0 biplanar).
  // Plane 0 is pure luma (Y)
  size_t width = CVPixelBufferGetWidth(pixbuf);
  size_t height = CVPixelBufferGetHeight(pixbuf);

  uint8_t *y_plane = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixbuf, 0);
  size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixbuf, 0);

  pthread_mutex_lock(&im->lock);

  int wi = im->write_idx;
  uint8_t *dst = im->gray_buf[wi];

  // Copy luma plane row-by-row
  for (size_t row = 0; row < height; row++) {
    memcpy(dst + row * width, y_plane + row * y_stride, width);
  }

  im->ready_idx = wi;
  im->has_frame = 1;
  im->write_idx = wi ^ 1; // swap to other slot
  pthread_cond_signal(&im->cond);
  pthread_mutex_unlock(&im->lock);

  CVPixelBufferUnlockBaseAddress(pixbuf, kCVPixelBufferLock_ReadOnly);
}

@end

int webcam_init(webcam_t *cam, const char *device, int width, int height) {
  struct webcam_impl *im = calloc(1, sizeof(struct webcam_impl));
  if (!im)
    return -1;

  pthread_mutex_init(&im->lock, NULL);
  pthread_cond_init(&im->cond, NULL);

  // Allocate two grayscale frame buffers
  im->buf_w = width;
  im->buf_h = height;
  for (int i = 0; i < FRAME_BUFS; i++) {
    im->gray_buf[i] = calloc((size_t)(width * height), 1);
    if (!im->gray_buf[i])
      goto fail;
  }

  // Find the camera device
  AVCaptureDevice *dev = nil;
  if (device) {
    // Match by localizedName
    NSString *devName = [NSString stringWithUTF8String:device];
    NSArray<AVCaptureDevice *> *devices =
        [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
    for (AVCaptureDevice *d in devices) {
      if ([d.localizedName isEqualToString:devName]) {
        dev = d;
        break;
      }
    }
  }
  if (!dev) {
    // Fall back to system default
    if (@available(macOS 10.15, *)) {
      AVCaptureDeviceDiscoverySession *ds = [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:@[
            AVCaptureDeviceTypeBuiltInWideAngleCamera
          ]
                                mediaType:AVMediaTypeVideo
                                 position:AVCaptureDevicePositionUnspecified];
      dev = ds.devices.firstObject;
    } else {
      dev = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    }
  }
  if (!dev) {
    goto fail;
  }

  // Configure format
  AVCaptureDeviceFormat *best_fmt = nil;
  float best_diff = 1e9f;
  for (AVCaptureDeviceFormat *fmt in dev.formats) {
    CMFormatDescriptionRef desc = fmt.formatDescription;
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(desc);
    float diff = (float)((dim.width - width) * (dim.width - width) +
                         (dim.height - height) * (dim.height - height));
    if (diff < best_diff) {
      best_diff = diff;
      best_fmt = fmt;
    }
  }
  if (best_fmt) {
    CMVideoDimensions dim =
        CMVideoFormatDescriptionGetDimensions(best_fmt.formatDescription);
    cam->width = dim.width;
    cam->height = dim.height;
    im->buf_w = cam->width;
    im->buf_h = cam->height;
    if (cam->width != width || cam->height != height) {
      for (int i = 0; i < FRAME_BUFS; i++) {
        free(im->gray_buf[i]);
        im->gray_buf[i] = calloc((size_t)(cam->width * cam->height), 1);
        if (!im->gray_buf[i])
          goto fail;
      }
    }
    if ([dev lockForConfiguration:nil]) {
      dev.activeFormat = best_fmt;
      [dev unlockForConfiguration];
    }
  } else {
    cam->width = width;
    cam->height = height;
  }

  im->session = [[AVCaptureSession alloc] init];
  [im->session beginConfiguration];
  im->session.sessionPreset = AVCaptureSessionPresetInputPriority;

  NSError *err = nil;
  im->input = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
  if (!im->input || err)
    goto fail_session;

  if (![im->session canAddInput:im->input])
    goto fail_session;
  [im->session addInput:im->input];

  im->output = [[AVCaptureVideoDataOutput alloc] init];
  im->output.videoSettings = @{
    (NSString *)kCVPixelBufferPixelFormatTypeKey :
        @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
  };
  im->output.alwaysDiscardsLateVideoFrames = YES;

  im->queue = dispatch_queue_create("asciicam.capture", DISPATCH_QUEUE_SERIAL);

  FrameDelegate *delegate = [[FrameDelegate alloc] init];
  delegate.impl = im;
  im->delegate = delegate;

  [im->output setSampleBufferDelegate:delegate queue:im->queue];

  if (![im->session canAddOutput:im->output])
    goto fail_session;
  [im->session addOutput:im->output];

  [im->session commitConfiguration];
  [im->session startRunning];

  cam->impl = im;
  cam->fd = -1;
  cam->buffer = NULL;
  return 0;

fail_session:
  [im->session commitConfiguration];
fail:
  for (int i = 0; i < FRAME_BUFS; i++)
    free(im->gray_buf[i]);
  pthread_mutex_destroy(&im->lock);
  pthread_cond_destroy(&im->cond);
  free(im);
  return -1;
}

int webcam_wait_frame(webcam_t *cam, int timeout_ms) {
  struct webcam_impl *im = cam->impl;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_ms / 1000;
  ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&im->lock);
  while (!im->has_frame && !im->stopped) {
    if (pthread_cond_timedwait(&im->cond, &im->lock, &ts) != 0) {
      pthread_mutex_unlock(&im->lock);
      return -1; // timeout
    }
  }
  int ok = im->has_frame;
  pthread_mutex_unlock(&im->lock);
  return ok ? 0 : -1;
}

int webcam_capture_frame(webcam_t *cam, uint8_t *gray_buffer) {
  struct webcam_impl *im = cam->impl;

  pthread_mutex_lock(&im->lock);
  if (!im->has_frame) {
    pthread_mutex_unlock(&im->lock);
    return -1;
  }

  int ri = im->ready_idx;
  im->has_frame = 0;
  pthread_mutex_unlock(&im->lock);

  // Copy the luma buffer out
  memcpy(gray_buffer, im->gray_buf[ri], (size_t)(im->buf_w * im->buf_h));
  return 0;
}

int webcam_requeue_buffer(webcam_t *cam) {
  (void)cam;
  return 0; // AVFoundation manages buffers
}

void webcam_cleanup(webcam_t *cam) {
  struct webcam_impl *im = cam->impl;
  if (!im)
    return;

  pthread_mutex_lock(&im->lock);
  im->stopped = 1;
  pthread_cond_broadcast(&im->cond);
  pthread_mutex_unlock(&im->lock);

  if (im->session) {
    [im->session stopRunning];
    im->session = nil;
    im->input = nil;
    im->output = nil;
    im->delegate = nil;
  }

  for (int i = 0; i < FRAME_BUFS; i++)
    free(im->gray_buf[i]);
  pthread_mutex_destroy(&im->lock);
  pthread_cond_destroy(&im->cond);
  free(im);

  cam->impl = NULL;
  cam->fd = -1;
  cam->buffer = NULL;
}

#endif /* PLATFORM_MACOS */
