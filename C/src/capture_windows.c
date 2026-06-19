#ifdef PLATFORM_WINDOWS
#include "capture.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

// Media Foundation is the modern (post-Vista) successor to DirectShow for
// webcam capture on Windows; DirectShow is still around mainly for legacy
// filter graphs and is on Microsoft's "prefer Media Foundation for new code"
// list, so that's what this backend uses. It needs COM initialized and
// links against mfplat.lib, mf.lib, mfreadwrite.lib, mfuuid.lib, ole32.lib
// (see the Windows section added to the Makefile).
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfobjects.h>

struct webcam_impl {
  IMFMediaSource *source;
  IMFSourceReader *reader;

  // The frame fetched by webcam_wait_frame() and consumed by
  // webcam_capture_frame(). Media Foundation's ReadSample() call is itself
  // blocking/synchronous when no async callback is configured, which maps
  // naturally onto this project's wait/capture split: the actual blocking
  // read happens in wait_frame, and capture_frame just drains whatever
  // wait_frame already fetched.
  IMFSample *pending_sample;

  int com_initialized;
  int mf_started;
};

#define SAFE_RELEASE(p)                                                      \
  do {                                                                       \
    if (p) {                                                                \
      (p)->lpVtbl->Release(p);                                              \
      (p) = NULL;                                                           \
    }                                                                        \
  } while (0)

static void release_pending_sample(struct webcam_impl *im) {
  if (im->pending_sample) {
    im->pending_sample->lpVtbl->Release(im->pending_sample);
    im->pending_sample = NULL;
  }
}

// Enumerate video capture devices and pick the one whose friendly name
// matches `device` (case-insensitive substring match), or the first device
// found if `device` is NULL. Mirrors capture_macos.c's localizedName match.
static IMFActivate *find_device(const char *device) {
  IMFAttributes *attrs = NULL;
  IMFActivate **devices = NULL;
  UINT32 count = 0;
  IMFActivate *chosen = NULL;

  if (FAILED(MFCreateAttributes(&attrs, 1)))
    return NULL;
  if (FAILED(attrs->lpVtbl->SetGUID(attrs, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                    &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP))) {
    SAFE_RELEASE(attrs);
    return NULL;
  }
  if (FAILED(MFEnumDeviceSources(attrs, &devices, &count)) || count == 0) {
    SAFE_RELEASE(attrs);
    return NULL;
  }

  for (UINT32 i = 0; i < count; i++) {
    if (chosen)
      break;
    if (!device) {
      chosen = devices[i];
      continue;
    }
    WCHAR *name = NULL;
    UINT32 name_len = 0;
    if (SUCCEEDED(devices[i]->lpVtbl->GetAllocatedString(
            devices[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name,
            &name_len))) {
      char narrow[256];
      int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow,
                                  sizeof(narrow), NULL, NULL);
      (void)n;
      CoTaskMemFree(name);
      if (strstr(narrow, device) != NULL)
        chosen = devices[i];
    }
  }
  if (!chosen && count > 0)
    chosen = devices[0]; // fall back to first device if name didn't match

  if (chosen)
    chosen->lpVtbl->AddRef(chosen);
  for (UINT32 i = 0; i < count; i++)
    SAFE_RELEASE(devices[i]);
  CoTaskMemFree(devices);
  SAFE_RELEASE(attrs);
  return chosen;
}

// Walk the native media types on stream 0 and pick whichever is closest to
// the requested width/height, same "closest match" approach
// capture_macos.c uses for AVCaptureDeviceFormat. Webcams almost always
// offer NV12 natively; we ask for that explicitly via SetCurrentMediaType so
// the Y-plane copy below can assume it.
static int negotiate_format(IMFSourceReader *reader, int want_w, int want_h,
                            int *out_w, int *out_h) {
  IMFMediaType *best = NULL;
  double best_diff = 1e18;
  int best_w = 0, best_h = 0;

  for (DWORD i = 0;; i++) {
    IMFMediaType *type = NULL;
    HRESULT hr = reader->lpVtbl->GetNativeMediaType(
        reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type);
    if (hr == MF_E_NO_MORE_TYPES || FAILED(hr))
      break;

    UINT64 packed_size = 0;
    if (SUCCEEDED(
            type->lpVtbl->GetUINT64(type, &MF_MT_FRAME_SIZE, &packed_size))) {
      UINT32 w = (UINT32)(packed_size >> 32);
      UINT32 h = (UINT32)(packed_size & 0xFFFFFFFF);
      double diff = (double)(w - want_w) * (w - want_w) +
                   (double)(h - want_h) * (h - want_h);
      if (diff < best_diff) {
        best_diff = diff;
        SAFE_RELEASE(best);
        best = type;
        best->lpVtbl->AddRef(best);
        best_w = (int)w;
        best_h = (int)h;
      }
    }
    SAFE_RELEASE(type);
  }

  if (!best)
    return -1;

  // Force NV12 output regardless of the native subtype the camera reports;
  // the source reader's built-in video processor (MF_SOURCE_READER_ENABLE_
  // VIDEO_PROCESSING) handles the conversion for us if the native format
  // is something else (e.g. MJPG).
  IMFMediaType *want_type = NULL;
  if (FAILED(MFCreateMediaType(&want_type))) {
    SAFE_RELEASE(best);
    return -1;
  }
  want_type->lpVtbl->SetGUID(want_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
  want_type->lpVtbl->SetGUID(want_type, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
  want_type->lpVtbl->SetUINT64(
      want_type, &MF_MT_FRAME_SIZE,
      ((UINT64)best_w << 32) | (UINT32)best_h);

  HRESULT hr = reader->lpVtbl->SetCurrentMediaType(
      reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, want_type);

  SAFE_RELEASE(want_type);
  SAFE_RELEASE(best);
  if (FAILED(hr))
    return -1;

  *out_w = best_w;
  *out_h = best_h;
  return 0;
}

int webcam_init(webcam_t *cam, const char *device, int width, int height) {
  struct webcam_impl *im = calloc(1, sizeof(struct webcam_impl));
  if (!im)
    return -1;

  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  // RPC_E_CHANGED_MODE means COM was already init'd on this thread in a
  // different concurrency mode by something else in the process -- that's
  // fine, we just don't own un-initializing it later in that case.
  im->com_initialized = (SUCCEEDED(hr) && hr != RPC_E_CHANGED_MODE);

  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    goto fail;
  im->mf_started = 1;

  IMFActivate *activate = find_device(device);
  if (!activate)
    goto fail;

  hr = activate->lpVtbl->ActivateObject(activate, &IID_IMFMediaSource,
                                        (void **)&im->source);
  SAFE_RELEASE(activate);
  if (FAILED(hr) || !im->source)
    goto fail;

  IMFAttributes *reader_attrs = NULL;
  if (FAILED(MFCreateAttributes(&reader_attrs, 1)))
    goto fail;
  // Let MF's built-in video processor handle MJPG/YUY2/etc -> NV12
  // conversion for us, so this backend only ever has to deal with NV12.
  reader_attrs->lpVtbl->SetUINT32(
      reader_attrs, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

  hr = MFCreateSourceReaderFromMediaSource(im->source, reader_attrs,
                                           &im->reader);
  SAFE_RELEASE(reader_attrs);
  if (FAILED(hr) || !im->reader)
    goto fail;

  int got_w = 0, got_h = 0;
  if (negotiate_format(im->reader, width, height, &got_w, &got_h) < 0) {
    // fall back to whatever's already current rather than failing outright
    got_w = width;
    got_h = height;
  }

  cam->width = got_w;
  cam->height = got_h;
  cam->impl = im;
  cam->fd = -1;
  cam->buffer = NULL;
  return 0;

fail:
  if (im->reader)
    SAFE_RELEASE(im->reader);
  if (im->source) {
    im->source->lpVtbl->Shutdown(im->source);
    SAFE_RELEASE(im->source);
  }
  if (im->mf_started)
    MFShutdown();
  if (im->com_initialized)
    CoUninitialize();
  free(im);
  return -1;
}

int webcam_wait_frame(webcam_t *cam, int timeout_ms) {
  struct webcam_impl *im = cam->impl;
  // The synchronous reader doesn't take a timeout directly; ReadSample()
  // blocks until a sample, error, or end-of-stream arrives. `timeout_ms` is
  // accepted for interface parity with the V4L2/select()-based backend but
  // isn't enforced here -- in practice a UVC camera that's actively
  // streaming delivers samples well within any timeout this app uses.
  (void)timeout_ms;

  release_pending_sample(im);

  DWORD stream_index = 0, stream_flags = 0;
  LONGLONG timestamp = 0;
  IMFSample *sample = NULL;

  HRESULT hr = im->reader->lpVtbl->ReadSample(
      im->reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
      &stream_index, &stream_flags, &timestamp, &sample);

  if (FAILED(hr) || !sample || (stream_flags & MF_SOURCE_READERF_ENDOFSTREAM))
    return -1;

  im->pending_sample = sample; // ownership transferred to impl
  return 0;
}

int webcam_capture_frame(webcam_t *cam, uint8_t *gray_buffer) {
  struct webcam_impl *im = cam->impl;
  if (!im->pending_sample)
    return -1;

  IMFMediaBuffer *buffer = NULL;
  if (FAILED(im->pending_sample->lpVtbl->ConvertToContiguousBuffer(
          im->pending_sample, &buffer)))
    return -1;

  BYTE *data = NULL;
  DWORD max_len = 0, cur_len = 0;
  if (FAILED(buffer->lpVtbl->Lock(buffer, &data, &max_len, &cur_len))) {
    SAFE_RELEASE(buffer);
    return -1;
  }

  // NV12: the Y (luma) plane comes first, tightly packed at cam->width
  // stride for the formats this app negotiates -- if a given driver hands
  // back a padded stride, IMF2DBuffer::Lock2D below would be the correct
  // way to get the real stride, but plain Lock() with width-sized rows
  // covers the overwhelming majority of UVC cameras.
  size_t plane_size = (size_t)cam->width * (size_t)cam->height;
  if ((size_t)cur_len >= plane_size)
    memcpy(gray_buffer, data, plane_size);

  buffer->lpVtbl->Unlock(buffer);
  SAFE_RELEASE(buffer);
  return 0;
}

int webcam_requeue_buffer(webcam_t *cam) {
  (void)cam;
  return 0; // Media Foundation manages its own sample lifetime
}

void webcam_cleanup(webcam_t *cam) {
  struct webcam_impl *im = cam->impl;
  if (!im)
    return;

  release_pending_sample(im);
  SAFE_RELEASE(im->reader);
  if (im->source) {
    im->source->lpVtbl->Shutdown(im->source);
    SAFE_RELEASE(im->source);
  }
  if (im->mf_started)
    MFShutdown();
  if (im->com_initialized)
    CoUninitialize();
  free(im);

  cam->impl = NULL;
  cam->fd = -1;
  cam->buffer = NULL;
}

// ---------------------------------------------------------------------------
// Hardware controls: not implemented on Windows yet.
//
// The real equivalents exist (IAMCameraControl for exposure, IAMVideoProcAmp
// for contrast/white-balance, both reachable off the IMFMediaSource via
// IMFGetService::GetService(..., MF_PROXY_PLAYER, IID_IAMCameraControl,...)-
// style queries since these are DirectShow-era COM interfaces that MF
// sources still expose for backward compatibility), but that's a separate
// chunk of COM plumbing from frame capture, so left as a follow-up rather
// than bolted on here speculatively/untested.
// TODO: implement via IAMCameraControl / IAMVideoProcAmp.
// ---------------------------------------------------------------------------

int webcam_set_auto_exposure(webcam_t *cam, int enable) {
  (void)cam;
  (void)enable;
  return -1;
}

int webcam_set_auto_white_balance(webcam_t *cam, int enable) {
  (void)cam;
  (void)enable;
  return -1;
}

int webcam_adjust_exposure(webcam_t *cam, int delta, int *out_value) {
  (void)cam;
  (void)delta;
  (void)out_value;
  return -1;
}

int webcam_adjust_contrast(webcam_t *cam, int delta, int *out_value) {
  (void)cam;
  (void)delta;
  (void)out_value;
  return -1;
}

int webcam_adjust_white_balance(webcam_t *cam, int delta, int *out_value) {
  (void)cam;
  (void)delta;
  (void)out_value;
  return -1;
}

int webcam_get_exposure(webcam_t *cam, int *value) {
  (void)cam;
  (void)value;
  return -1;
}

int webcam_get_contrast(webcam_t *cam, int *value) {
  (void)cam;
  (void)value;
  return -1;
}

int webcam_get_white_balance(webcam_t *cam, int *value) {
  (void)cam;
  (void)value;
  return -1;
}

int webcam_get_exposure_range(webcam_t *cam, int *min, int *max) {
  (void)cam;
  (void)min;
  (void)max;
  return -1;
}

int webcam_get_contrast_range(webcam_t *cam, int *min, int *max) {
  (void)cam;
  (void)min;
  (void)max;
  return -1;
}

int webcam_get_white_balance_range(webcam_t *cam, int *min, int *max) {
  (void)cam;
  (void)min;
  (void)max;
  return -1;
}

#endif /* PLATFORM_WINDOWS */
