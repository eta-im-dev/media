/* GStreamer
 * Copyright (C) 2005 Sebastien Moutte <sebastien@moutte.net>
 * Copyright (C) 2007-2009 Pioneers of the Inevitable <songbird@songbirdnest.com>
 * Copyright (C) 2009 Barracuda Networks, Inc.
 *
 * gstdirectsoundringbuffer.c:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * The development of this code was made possible due to the involvement
 * of Pioneers of the Inevitable, the creators of the Songbird Music player
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstdirectsoundringbuffer.h"

#include "gstdirectsoundsink.h"
#include "gstdirectsoundsrc.h"

#define GST_CAT_DEFAULT directsound

#define MAX_LOST_RETRIES 10
#define DIRECTSOUND_ERROR_DEVICE_RECONFIGURED 0x88780096
#define DIRECTSOUND_ERROR_DEVICE_NO_DRIVER    0x88780078

static void gst_directsound_ring_buffer_class_init (
    GstDirectSoundRingBufferClass * klass);
static void gst_directsound_ring_buffer_init (
    GstDirectSoundRingBuffer * ringbuffer,
    GstDirectSoundRingBufferClass * g_class);
static void gst_directsound_ring_buffer_dispose (GObject * object);
static void gst_directsound_ring_buffer_finalize (GObject * object);
static gboolean gst_directsound_ring_buffer_open_device (GstRingBuffer * buf);
static gboolean gst_directsound_ring_buffer_close_device (
    GstRingBuffer * buf);

static gboolean gst_directsound_ring_buffer_acquire (GstRingBuffer * buf,
    GstRingBufferSpec * spec);
static gboolean gst_directsound_ring_buffer_release (GstRingBuffer * buf);

static gboolean gst_directsound_ring_buffer_start (GstRingBuffer * buf);
static gboolean gst_directsound_ring_buffer_pause (GstRingBuffer * buf);
static gboolean gst_directsound_ring_buffer_resume (GstRingBuffer * buf);
static gboolean gst_directsound_ring_buffer_stop (GstRingBuffer * buf);
static guint gst_directsound_ring_buffer_delay (GstRingBuffer * buf);
static GstRingBufferClass * ring_parent_class = NULL;

static DWORD WINAPI gst_directsound_write_proc (LPVOID lpParameter);
static DWORD WINAPI gst_directsound_read_proc (LPVOID lpParameter);

GST_BOILERPLATE (GstDirectSoundRingBuffer, gst_directsound_ring_buffer,
    GstRingBuffer, GST_TYPE_RING_BUFFER);

static void
gst_directsound_ring_buffer_base_init (gpointer g_class)
{
  /* Nothing to do right now */
}

static void
gst_directsound_ring_buffer_class_init (GstDirectSoundRingBufferClass * klass)
{
  GObjectClass * gobject_class;
  GstObjectClass * gstobject_class;
  GstRingBufferClass * gstringbuffer_class;

  gobject_class = (GObjectClass *) klass;
  gstobject_class = (GstObjectClass *) klass;
  gstringbuffer_class = (GstRingBufferClass *) klass;

  ring_parent_class = g_type_class_peek_parent (klass);

  gobject_class->dispose =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_dispose);
  gobject_class->finalize =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_finalize);

  gstringbuffer_class->open_device =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_open_device);
  gstringbuffer_class->close_device =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_close_device);
  gstringbuffer_class->acquire =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_acquire);
  gstringbuffer_class->release =
      GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_release);
  gstringbuffer_class->start = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_start);
  gstringbuffer_class->pause = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_pause);
  gstringbuffer_class->resume = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_resume);
  gstringbuffer_class->stop = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_stop);

  gstringbuffer_class->delay = GST_DEBUG_FUNCPTR (gst_directsound_ring_buffer_delay);

  GST_DEBUG ("directsound ring buffer class init");
}

static void
gst_directsound_ring_buffer_init (GstDirectSoundRingBuffer * ringbuffer,
    GstDirectSoundRingBufferClass * g_class)
{
  ringbuffer->element = NULL;
  ringbuffer->pDS8 = NULL;
  ringbuffer->pDSB8 = NULL;
  ringbuffer->pDSC8 = NULL;
  ringbuffer->pDSCB8 = NULL;

  memset (&ringbuffer->wave_format, 0, sizeof (WAVEFORMATEX));

  ringbuffer->buffer_size = 0;
  ringbuffer->buffer_circular_offset = 0;

  ringbuffer->min_buffer_size = 0;
  ringbuffer->min_sleep_time = 10; /* in milliseconds */

  ringbuffer->bytes_per_sample = 0;
  ringbuffer->segoffset = 0;
  ringbuffer->segsize = 0;

  ringbuffer->hThread = NULL;
  ringbuffer->suspended = FALSE;
  ringbuffer->should_run = FALSE;
  ringbuffer->flushing = FALSE;

  ringbuffer->volume = 1.0;

  ringbuffer->dsound_lock = g_mutex_new ();
}

static void
gst_directsound_ring_buffer_dispose (GObject * object)
{
  G_OBJECT_CLASS (ring_parent_class)->dispose (object);
}

static void
gst_directsound_ring_buffer_finalize (GObject * object)
{
  GstDirectSoundRingBuffer * dsoundbuffer =
     GST_DIRECTSOUND_RING_BUFFER (object);

  g_mutex_free (dsoundbuffer->dsound_lock);
  dsoundbuffer->dsound_lock = NULL;

  G_OBJECT_CLASS (ring_parent_class)->finalize (object);
}

static gboolean
gst_directsound_ring_buffer_open_device (GstRingBuffer * buf)
{
  HRESULT hr;
  gchar * device_id;
  LPGUID lpGuid = NULL;
  GstDirectSoundRingBuffer * dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DEBUG ("Opening DirectSound Device");

  if (dsoundbuffer->is_src) {
    device_id = ((GstDirectSoundSrc *)(dsoundbuffer->element))->device_id;
    if (device_id)
      lpGuid = gst_directsound_get_device_guid (device_id);

    if (FAILED (hr = DirectSoundCaptureCreate8 (lpGuid, &dsoundbuffer->pDSC8, NULL))) {
      GST_ELEMENT_ERROR (dsoundbuffer->element, RESOURCE, FAILED,
        ("%ls.", DXGetErrorDescription9W(hr)),
        ("Failed to create directsound device. (%X)", (unsigned int) hr));
      dsoundbuffer->pDSC8 = NULL;
      if (lpGuid)
        g_free (lpGuid);
      return FALSE;
    }

    if (lpGuid)
      g_free (lpGuid);
  }
  else {
    device_id = ((GstDirectSoundSink *)(dsoundbuffer->element))->device_id;
    if (device_id)
      lpGuid = gst_directsound_get_device_guid (device_id);

    if (FAILED (hr = DirectSoundCreate8 (lpGuid, &dsoundbuffer->pDS8, NULL))) {
      GST_ELEMENT_ERROR (dsoundbuffer->element, RESOURCE, FAILED,
        ("%ls.", DXGetErrorDescription9W(hr)),
        ("Failed to create directsound device. (%X)", (unsigned int) hr));
      dsoundbuffer->pDS8 = NULL;
      if (lpGuid)
        g_free (lpGuid);
      return FALSE;
    }

    if (lpGuid)
      g_free (lpGuid);

    if (FAILED (hr = IDirectSound8_SetCooperativeLevel (dsoundbuffer->pDS8,
                GetDesktopWindow (), DSSCL_PRIORITY))) {
      GST_WARNING ("gst_directsound_sink_open: IDirectSound8_SetCooperativeLevel, hr = %X", (unsigned int) hr);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_close_device (GstRingBuffer * buf)
{
  GstDirectSoundRingBuffer * dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DEBUG ("Closing DirectSound Device");

  if (dsoundbuffer->is_src) {
    if (dsoundbuffer->pDSC8) {
      IDirectSoundCapture_Release (dsoundbuffer->pDSC8);
      dsoundbuffer->pDSC8 = NULL;
    }
  }
  else {
    if (dsoundbuffer->pDS8) {
      IDirectSound8_Release (dsoundbuffer->pDS8);
      dsoundbuffer->pDS8 = NULL;
    }
  }

  return TRUE;
}

static gboolean
gst_directsound_create_buffer (GstRingBuffer * buf) 
{
  GstDirectSoundRingBuffer * dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);
  HRESULT hr;
  DSBUFFERDESC descSecondary;
  LPDIRECTSOUNDBUFFER pDSB;
  DSCBUFFERDESC captureDescSecondary;
  LPDIRECTSOUNDCAPTUREBUFFER pDSCB;

  if (dsoundbuffer->is_src) {
    memset (&captureDescSecondary, 0, sizeof (DSCBUFFERDESC));
    captureDescSecondary.dwSize = sizeof (DSCBUFFERDESC);
    captureDescSecondary.dwFlags = 0;

    captureDescSecondary.dwBufferBytes = dsoundbuffer->buffer_size;
    captureDescSecondary.lpwfxFormat = (WAVEFORMATEX *) &dsoundbuffer->wave_format;

    hr = IDirectSoundCapture_CreateCaptureBuffer (dsoundbuffer->pDSC8, &captureDescSecondary,
        &pDSCB, NULL);
    if (G_UNLIKELY (FAILED (hr))) {
      GST_WARNING ("gst_directsound_ring_buffer_acquire: IDirectSoundCapture_CreateCaptureBuffer, hr = %X", (unsigned int) hr);
      return FALSE;
    }

    hr = IDirectSoundCaptureBuffer_QueryInterface (pDSCB,
        &IID_IDirectSoundCaptureBuffer8, (LPVOID *) &dsoundbuffer->pDSCB8);
    if (G_UNLIKELY (FAILED (hr))) {
      IDirectSoundCaptureBuffer_Release (pDSCB);
      GST_WARNING ("gst_directsound_ring_buffer_acquire: IDirectSoundCaptureBuffer_QueryInterface, hr = %X", (unsigned int) hr);
      return FALSE;
    }

    IDirectSoundCaptureBuffer_Release (pDSCB);
  }
  else {
    memset (&descSecondary, 0, sizeof (DSBUFFERDESC));
    descSecondary.dwSize = sizeof (DSBUFFERDESC);
    descSecondary.dwFlags = DSBCAPS_GETCURRENTPOSITION2 |
        DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME;

    descSecondary.dwBufferBytes = dsoundbuffer->buffer_size;
    descSecondary.lpwfxFormat = (WAVEFORMATEX *) &dsoundbuffer->wave_format;

    hr = IDirectSound8_CreateSoundBuffer (dsoundbuffer->pDS8, &descSecondary,
        &pDSB, NULL);
    if (G_UNLIKELY (FAILED (hr))) {
      GST_WARNING ("gst_directsound_ring_buffer_acquire: IDirectSound8_CreateSoundBuffer, hr = %X", (unsigned int) hr);
      return FALSE;
    }

    hr = IDirectSoundBuffer_QueryInterface (pDSB, &IID_IDirectSoundBuffer8,
        (LPVOID *) &dsoundbuffer->pDSB8);
    if (G_UNLIKELY (FAILED (hr))) {
      IDirectSoundBuffer_Release (pDSB);
      GST_WARNING ("gst_directsound_ring_buffer_acquire: IDirectSoundBuffer_QueryInterface, hr = %X", (unsigned int) hr);
      return FALSE;
    }

    IDirectSoundBuffer_Release (pDSB);
  }

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_acquire (GstRingBuffer * buf, GstRingBufferSpec * spec)
{
  GstDirectSoundRingBuffer * dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);
  WAVEFORMATEX wfx;

  /* sanity check, if no DirectSound device, bail out */
  if (!dsoundbuffer->pDS8 && !dsoundbuffer->pDSC8) {
    GST_WARNING ("gst_directsound_ring_buffer_acquire: DirectSound 8 device is null!");
    return FALSE;
  }

  /* save number of bytes per sample */
  dsoundbuffer->bytes_per_sample = spec->bytes_per_sample;

  /* fill the WAVEFORMATEX structure with spec params */
  memset (&wfx, 0, sizeof (wfx));
  wfx.cbSize = sizeof (wfx);
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = spec->channels;
  wfx.nSamplesPerSec = spec->rate;
  wfx.wBitsPerSample = (spec->bytes_per_sample * 8) / wfx.nChannels;
  wfx.nBlockAlign = spec->bytes_per_sample;
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  /* enforce a minimum latency of 2x the sleep_time */
  if (spec->latency_time < (dsoundbuffer->min_sleep_time * 2 * 1000))
    spec->latency_time = dsoundbuffer->min_sleep_time * 2 * 1000;

  /* Create directsound buffer with size based on our configured
   * buffer_size (which is 200 ms by default) */
  dsoundbuffer->buffer_size = gst_util_uint64_scale_int (wfx.nAvgBytesPerSec, spec->buffer_time, GST_MSECOND);

  spec->segsize = gst_util_uint64_scale_int (wfx.nAvgBytesPerSec, spec->latency_time, GST_MSECOND);
  /* Now round the ringbuffer segment size to a multiple of the bytes per sample -
     otherwise the ringbuffer subtly fails */
  spec->segsize = (spec->segsize + (spec->bytes_per_sample - 1))/ spec->bytes_per_sample * spec->bytes_per_sample;

  /* And base the total number of segments on the configured buffer size */
  spec->segtotal = dsoundbuffer->buffer_size / spec->segsize;

  dsoundbuffer->buffer_size = spec->segsize * spec->segtotal;
  dsoundbuffer->segsize = spec->segsize;
  dsoundbuffer->min_buffer_size = dsoundbuffer->buffer_size / 2;

  GST_INFO_OBJECT (dsoundbuffer,
      "GstRingBufferSpec->channels: %d, GstRingBufferSpec->rate: %d, GstRingBufferSpec->bytes_per_sample: %d\n"
      "WAVEFORMATEX.nSamplesPerSec: %ld, WAVEFORMATEX.wBitsPerSample: %d, WAVEFORMATEX.nBlockAlign: %d, WAVEFORMATEX.nAvgBytesPerSec: %ld\n"
      "Size of dsound cirucular buffer: %d, Size of segment: %d, Total segments: %d\n", spec->channels, spec->rate,
      spec->bytes_per_sample, wfx.nSamplesPerSec, wfx.wBitsPerSample,
      wfx.nBlockAlign, wfx.nAvgBytesPerSec, dsoundbuffer->buffer_size, spec->segsize, spec->segtotal);

  dsoundbuffer->wave_format = wfx;

  if (!gst_directsound_create_buffer (buf))
    return FALSE;

  buf->data = gst_buffer_new_and_alloc (spec->segtotal * spec->segsize);
  memset (GST_BUFFER_DATA (buf->data), 0, GST_BUFFER_SIZE (buf->data));

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_release (GstRingBuffer * buf)
{
  GstDirectSoundRingBuffer * dsoundbuffer;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  /* first we have to ensure our ring buffer is stopped */
  gst_directsound_ring_buffer_stop (buf);

  GST_DSOUND_LOCK (dsoundbuffer);

  /* release secondary DirectSound buffer */

  if (dsoundbuffer->pDSB8) {
    IDirectSoundBuffer8_Release (dsoundbuffer->pDSB8);
    dsoundbuffer->pDSB8 = NULL;
  }

  if (dsoundbuffer->pDSCB8) {
    IDirectSoundCaptureBuffer8_Release (dsoundbuffer->pDSCB8);
    dsoundbuffer->pDSCB8 = NULL;
  }

  gst_buffer_unref (buf->data);
  buf->data = NULL;

  GST_DSOUND_UNLOCK (dsoundbuffer);

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_start (GstRingBuffer * buf)
{
  GstDirectSoundRingBuffer * dsoundbuffer;
  HANDLE hThread;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DEBUG ("Starting RingBuffer");

  GST_DSOUND_LOCK (dsoundbuffer);

  if (dsoundbuffer->is_src) {
    hThread = CreateThread (NULL, 256 * 1024 /* Stack size: 256k */,
       gst_directsound_read_proc, buf, CREATE_SUSPENDED, NULL);
  }
  else {
    hThread = CreateThread (NULL, 256 * 1024 /* Stack size: 256k */,
       gst_directsound_write_proc, buf, CREATE_SUSPENDED, NULL);
  }

  if (!hThread) {
    GST_DSOUND_UNLOCK (dsoundbuffer);
    GST_WARNING ("gst_directsound_ring_buffer_start: CreateThread");
    return FALSE;
  }

  dsoundbuffer->hThread = hThread;
  dsoundbuffer->should_run = TRUE;

  if (!dsoundbuffer->is_src)
    gst_directsound_set_volume (dsoundbuffer->pDSB8, dsoundbuffer->volume);

  if (G_UNLIKELY (!SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL)))
    GST_WARNING ("gst_directsound_ring_buffer_start: Failed to set thread priority.");

  ResumeThread (hThread);

  GST_DSOUND_UNLOCK (dsoundbuffer);

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_pause (GstRingBuffer * buf)
{
  HRESULT hr = S_OK;
  GstDirectSoundRingBuffer * dsoundbuffer;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DEBUG ("Pausing RingBuffer");

  GST_DSOUND_LOCK (dsoundbuffer);

  if (dsoundbuffer->is_src) {
    if (dsoundbuffer->pDSCB8) {
      hr = IDirectSoundCaptureBuffer_Stop (dsoundbuffer->pDSCB8);
    }
  }
  else {
    if (dsoundbuffer->pDSB8) {
      hr = IDirectSoundBuffer8_Stop (dsoundbuffer->pDSB8);
    }
  }

  if (G_LIKELY (!dsoundbuffer->suspended)) {
    if (G_UNLIKELY(SuspendThread (dsoundbuffer->hThread) == -1))
      GST_WARNING ("gst_directsound_ring_buffer_pause: SuspendThread failed.");
    else 
      dsoundbuffer->suspended = TRUE;
  }

  GST_DSOUND_UNLOCK (dsoundbuffer);

  /* in the unlikely event that a device was reconfigured, we can consider
   * ourselves stopped even though the stop call failed */
  if (G_UNLIKELY (FAILED(hr)) &&
      G_UNLIKELY(hr != DIRECTSOUND_ERROR_DEVICE_RECONFIGURED) &&
      G_UNLIKELY(hr != DIRECTSOUND_ERROR_DEVICE_NO_DRIVER)) {
    if (dsoundbuffer->is_src)
      GST_WARNING ("gst_directsound_ring_buffer_pause: IDirectSoundCaptureBuffer_Stop, hr = %X", (unsigned int) hr);
    else
      GST_WARNING ("gst_directsound_ring_buffer_pause: IDirectSoundBuffer8_Stop, hr = %X", (unsigned int) hr);
    return FALSE;
  }

  return TRUE;
}

static gboolean 
gst_directsound_ring_buffer_resume (GstRingBuffer * buf)
{
  GstDirectSoundRingBuffer * dsoundbuffer;
  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DEBUG ("Resuming RingBuffer");

  GST_DSOUND_LOCK (dsoundbuffer);

  if (G_LIKELY (dsoundbuffer->suspended) &&
      ResumeThread (dsoundbuffer->hThread) != -1) {
    dsoundbuffer->suspended = FALSE;
  }
  else {
    GST_DSOUND_UNLOCK (dsoundbuffer);
    GST_WARNING ("gst_directsound_ring_buffer_resume: ResumeThread failed.");
    return FALSE;
  }

  GST_DSOUND_UNLOCK (dsoundbuffer);

  return TRUE;
}

static gboolean
gst_directsound_ring_buffer_stop (GstRingBuffer * buf)
{
  HRESULT hr;
  DWORD ret;
  HANDLE hThread;
  GstDirectSoundRingBuffer * dsoundbuffer;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DEBUG ("Stopping RingBuffer");

  GST_DSOUND_LOCK (dsoundbuffer);
  dsoundbuffer->should_run = FALSE;

  if (dsoundbuffer->is_src) {
    if (dsoundbuffer->pDSCB8) {
      hr = IDirectSoundCaptureBuffer_Stop (dsoundbuffer->pDSCB8);

      if (G_UNLIKELY (FAILED(hr))) {
        GST_DSOUND_UNLOCK (dsoundbuffer);
        GST_WARNING ("gst_directsound_ring_buffer_stop: IDirectSoundCaptureBuffer_Stop, hr = %X", (unsigned int) hr);
        return FALSE;
      }
    }
  }
  else {
    if (dsoundbuffer->pDSB8) {
      hr = IDirectSoundBuffer8_Stop (dsoundbuffer->pDSB8);

      if (G_UNLIKELY (FAILED(hr))) {
        GST_DSOUND_UNLOCK (dsoundbuffer);
        GST_WARNING ("gst_directsound_ring_buffer_stop: IDirectSoundBuffer8_Stop, hr = %X", (unsigned int) hr);
        return FALSE;
      }
    }
  }

  hThread = dsoundbuffer->hThread;

  if (dsoundbuffer->suspended &&
      ResumeThread (hThread) != -1) {
    dsoundbuffer->suspended = FALSE;
  }
  else {
    GST_DSOUND_UNLOCK (dsoundbuffer);
    GST_WARNING ("gst_directsound_ring_buffer_stop: ResumeThread failed.");
    return FALSE;
  }

  GST_DSOUND_UNLOCK (dsoundbuffer);

  /* wait without lock held */
  ret = WaitForSingleObject (hThread, 5000);

  if (G_UNLIKELY (ret == WAIT_TIMEOUT)) {
    GST_WARNING ("gst_directsound_ring_buffer_stop: Failed to wait for thread shutdown. (%u)", (unsigned int) ret);
    return FALSE;
  }

  GST_DSOUND_LOCK (dsoundbuffer);
  CloseHandle (dsoundbuffer->hThread);
  dsoundbuffer->hThread = NULL;
  GST_DSOUND_UNLOCK (dsoundbuffer);

  return TRUE;
}

static guint
gst_directsound_ring_buffer_delay (GstRingBuffer * buf)
{
  GstDirectSoundRingBuffer * dsoundbuffer;
  HRESULT hr;
  DWORD dwCurrentPlayCursor;
  DWORD dwCurrentWriteCursor;
  DWORD dwCurrentCaptureCursor;
  DWORD dwCurrentReadCursor;
  DWORD dwBytesInQueue = 0;
  gint nNbSamplesInQueue = 0;

  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  GST_DSOUND_LOCK (dsoundbuffer);

  if (dsoundbuffer->is_src) {
    if (G_LIKELY (dsoundbuffer->pDSCB8)) {
      /* evaluate the number of samples in queue in the circular buffer */
      hr = IDirectSoundCaptureBuffer8_GetCurrentPosition (
          dsoundbuffer->pDSCB8, &dwCurrentCaptureCursor,
          &dwCurrentReadCursor);

      if (G_LIKELY (SUCCEEDED (hr))) {
        if (dwCurrentCaptureCursor >= dsoundbuffer->buffer_circular_offset)
          dwBytesInQueue = dwCurrentCaptureCursor -
              dsoundbuffer->buffer_circular_offset;
        else
          dwBytesInQueue = dwCurrentCaptureCursor +
              (dsoundbuffer->buffer_size -
              dsoundbuffer->buffer_circular_offset);

        nNbSamplesInQueue = dwBytesInQueue / dsoundbuffer->bytes_per_sample;
      }
      else {
        GST_WARNING ("gst_directsound_ring_buffer_delay: IDirectSoundCaptureBuffer8_GetCurrentPosition, hr = %X", (unsigned int) hr);
      }
    }
  }
  else {
    if (G_LIKELY (dsoundbuffer->pDSB8)) {
      /* evaluate the number of samples in queue in the circular buffer */
      hr = IDirectSoundBuffer8_GetCurrentPosition (dsoundbuffer->pDSB8,
          &dwCurrentPlayCursor, &dwCurrentWriteCursor);

      if (G_LIKELY (SUCCEEDED (hr))) {
        if (dwCurrentPlayCursor <= dsoundbuffer->buffer_circular_offset)
          dwBytesInQueue = dsoundbuffer->buffer_circular_offset -
              dwCurrentPlayCursor;
        else
          dwBytesInQueue = dsoundbuffer->buffer_circular_offset +
              (dsoundbuffer->buffer_size - dwCurrentPlayCursor);

        nNbSamplesInQueue = dwBytesInQueue / dsoundbuffer->bytes_per_sample;
      }
      else {
        GST_WARNING ("gst_directsound_ring_buffer_delay: IDirectSoundBuffer8_GetCurrentPosition, hr = %X", (unsigned int) hr);
      }
    }
  }

  GST_DSOUND_UNLOCK (dsoundbuffer);

  return nNbSamplesInQueue;
}

static DWORD WINAPI
gst_directsound_write_proc (LPVOID lpParameter)
{
  GstRingBuffer * buf;
  GstDirectSoundRingBuffer * dsoundbuffer;

  HRESULT hr;
  DWORD dwStatus;
  LPVOID pLockedBuffer1 = NULL, pLockedBuffer2 = NULL;
  DWORD dwSizeBuffer1 = 0, dwSizeBuffer2 = 0;
  DWORD dwCurrentPlayCursor = 0;

  gint64 freeBufferSize = 0;

  guint8 * readptr = NULL;
  gint readseg = 0;
  guint len = 0;
  gint retries = 0;

  gboolean flushing = FALSE;
  gboolean should_run = TRUE;
  gboolean error = FALSE;

  buf = (GstRingBuffer *) lpParameter;
  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  do {
    GST_DSOUND_LOCK (dsoundbuffer);

    if (dsoundbuffer->flushing || !dsoundbuffer->pDSB8) {
      GST_DSOUND_UNLOCK (dsoundbuffer);
      goto complete;
    }

    GST_DSOUND_UNLOCK (dsoundbuffer);

  restore_buffer:
    /* get current buffer status */
    GST_DSOUND_LOCK (dsoundbuffer);
    hr = IDirectSoundBuffer8_GetStatus (dsoundbuffer->pDSB8, &dwStatus);
    GST_DSOUND_UNLOCK (dsoundbuffer);

    if (dwStatus & DSBSTATUS_BUFFERLOST) {
      GST_DEBUG ("Buffer was lost, attempting to restore");

      GST_DSOUND_LOCK (dsoundbuffer);
      hr = IDirectSoundBuffer8_Restore (dsoundbuffer->pDSB8);
      GST_DSOUND_UNLOCK (dsoundbuffer);

      /* restore may fail again, ensure we restore the
       * buffer before we continue */
      if (FAILED(hr) && hr == DSERR_BUFFERLOST) {
        if (retries++ < MAX_LOST_RETRIES) {
          GST_DEBUG ("Unable to restore, trying again");
          goto restore_buffer;
        }
        else {
          GST_ELEMENT_ERROR (dsoundbuffer->element, RESOURCE, FAILED,
             ("%ls.", DXGetErrorDescription9W(hr)),
             ("gst_directsound_write_proc: IDirectSoundBuffer8_Restore, hr = %X", (unsigned int) hr));
          goto complete;
        }
      }
    }

    /* get current play cursor and write cursor positions */
    GST_DSOUND_LOCK (dsoundbuffer);
    hr = IDirectSoundBuffer8_GetCurrentPosition (dsoundbuffer->pDSB8,
        &dwCurrentPlayCursor, NULL);
    GST_DSOUND_UNLOCK (dsoundbuffer);

    if (G_UNLIKELY (FAILED(hr))) {
      /* try and reopen the default directsound device */
      if (hr == DIRECTSOUND_ERROR_DEVICE_RECONFIGURED) {
        /* we have to wait a while for the sound device removal to actually
         * be processed before attempting to reopen the device. Yes, this
         * sucks */
        Sleep (2000);

        GST_DSOUND_LOCK (dsoundbuffer);
        IDirectSoundBuffer8_Release (dsoundbuffer->pDSB8);
        dsoundbuffer->pDSB8 = NULL;
        GST_DSOUND_UNLOCK (dsoundbuffer);

        if (gst_directsound_ring_buffer_close_device (buf) &&
            gst_directsound_ring_buffer_open_device (buf) &&
            gst_directsound_create_buffer (buf) ) {
          dsoundbuffer->buffer_circular_offset = 0;
          goto restore_buffer;
        }
      }

      /* only trigger an error if we're not already in an error state */
      if (FAILED(hr) && !error) {
        GST_ELEMENT_ERROR (dsoundbuffer->element, RESOURCE, FAILED,
           ("%ls.", DXGetErrorDescription9W(hr)),
           ("gst_directsound_write_proc: IDirectSoundBuffer8_GetCurrentPosition, hr = %X", (unsigned int) hr));
        error = TRUE;
        goto complete;
      }
    }

    GST_LOG ("Current Play Cursor: %u Current Write Offset: %d",
        (unsigned int) dwCurrentPlayCursor,
        dsoundbuffer->buffer_circular_offset);

    /* calculate the free size of the circular buffer */
    GST_DSOUND_LOCK (dsoundbuffer);
    if (dwCurrentPlayCursor <= dsoundbuffer->buffer_circular_offset)
      freeBufferSize = dsoundbuffer->buffer_size -
        (dsoundbuffer->buffer_circular_offset - dwCurrentPlayCursor);
    else
      freeBufferSize = dwCurrentPlayCursor - dsoundbuffer->buffer_circular_offset;
    GST_DSOUND_UNLOCK (dsoundbuffer);

    if (!gst_ring_buffer_prepare_read (buf, &readseg, &readptr, &len))
      goto complete;

    len -= dsoundbuffer->segoffset;

    GST_LOG ("Size of segment to write: %d Free buffer size: %lld", len,
        freeBufferSize);

    /* If we can't write this into directsound because we don't have enough
     * space, then start playback if we're currently paused. Then, sleep
     * for a little while to wait until space is available */
    // ###: why >= and not > ?
    // ###: what happens if this condition is true on the first iteration?
    //   playback totally busted?
    if (len >= freeBufferSize) {
      if (!(dwStatus & DSBSTATUS_PLAYING)) {
        GST_DSOUND_LOCK (dsoundbuffer);
        hr = IDirectSoundBuffer8_Play (dsoundbuffer->pDSB8, 0, 0, DSBPLAY_LOOPING);
        GST_DSOUND_UNLOCK (dsoundbuffer);

        if (FAILED(hr)) {
          GST_WARNING ("gst_directsound_write_proc: IDirectSoundBuffer8_Play, hr = %X", (unsigned int) hr);
        }
      }

      goto complete;
    }

    /* lock it */
    GST_DSOUND_LOCK (dsoundbuffer);
    hr = IDirectSoundBuffer8_Lock (dsoundbuffer->pDSB8,
        dsoundbuffer->buffer_circular_offset, len, &pLockedBuffer1,
        &dwSizeBuffer1, &pLockedBuffer2, &dwSizeBuffer2, 0L);

    /* copy chunks */
    if (SUCCEEDED (hr)) {
      // ###: is it possible for len > dwSizeBuffer1 + dwSizeBuffer2 ?
      if (len <= dwSizeBuffer1) {
        memcpy (pLockedBuffer1, (LPBYTE) readptr + dsoundbuffer->segoffset, len);
      }
      else {
        memcpy (pLockedBuffer1, (LPBYTE) readptr + dsoundbuffer->segoffset, dwSizeBuffer1);
        memcpy (pLockedBuffer2, (LPBYTE) readptr + dsoundbuffer->segoffset + dwSizeBuffer1, len - dwSizeBuffer1);
      }

      IDirectSoundBuffer8_Unlock (dsoundbuffer->pDSB8, pLockedBuffer1,
         dwSizeBuffer1, pLockedBuffer2, dwSizeBuffer2);
    }
    else {
      GST_WARNING ("gst_directsound_write_proc: IDirectSoundBuffer8_Lock, hr = %X", (unsigned int) hr);
    }

    /* update tracking data */
    dsoundbuffer->segoffset += dwSizeBuffer1 + (len - dwSizeBuffer1);

    dsoundbuffer->buffer_circular_offset += dwSizeBuffer1 + (len - dwSizeBuffer1);
    dsoundbuffer->buffer_circular_offset %= dsoundbuffer->buffer_size;
    GST_DSOUND_UNLOCK (dsoundbuffer);

    freeBufferSize -= dwSizeBuffer1 + (len - dwSizeBuffer1);

    GST_LOG ("DirectSound Buffer1 Data Size: %u DirectSound Buffer2 Data Size: %u",
        (unsigned int) dwSizeBuffer1, (unsigned int) dwSizeBuffer2);
    GST_LOG ("Free buffer size: %lld", freeBufferSize);

    /* check if we read a whole segment */
    GST_DSOUND_LOCK (dsoundbuffer);
    if (dsoundbuffer->segoffset == dsoundbuffer->segsize) {
      GST_DSOUND_UNLOCK (dsoundbuffer);

      /* advance to next segment */
      gst_ring_buffer_clear (buf, readseg);
      gst_ring_buffer_advance (buf, 1);

      GST_DSOUND_LOCK (dsoundbuffer);
      dsoundbuffer->segoffset = 0;
    }
    GST_DSOUND_UNLOCK (dsoundbuffer);

  complete:
    GST_DSOUND_LOCK (dsoundbuffer);

    should_run = dsoundbuffer->should_run;
    flushing = dsoundbuffer->flushing;
    retries = 0;

    GST_DSOUND_UNLOCK (dsoundbuffer);

    /* it's extremely important to sleep in without the lock! */
    // ###: why >= and not > ?
    if (len >= freeBufferSize || flushing || error)
      Sleep (dsoundbuffer->min_sleep_time);
  }
  while(should_run);

  return 0;
}

static DWORD WINAPI
gst_directsound_read_proc (LPVOID lpParameter)
{
  GstRingBuffer * buf;
  GstDirectSoundRingBuffer * dsoundbuffer;

  HRESULT hr;
  DWORD dwStatus;
  LPVOID pLockedBuffer1 = NULL, pLockedBuffer2 = NULL;
  DWORD dwSizeBuffer1 = 0, dwSizeBuffer2 = 0;
  DWORD dwCurrentCaptureCursor = 0;
  DWORD dwCurrentReadCursor = 0;

  gint64 capturedBufferSize = 0;

  guint8 * writeptr = NULL;
  gint writeseg = 0;
  guint len = 0;
  gint retries = 0;

  gboolean flushing = FALSE;
  gboolean should_run = TRUE;
  gboolean error = FALSE;

  buf = (GstRingBuffer *) lpParameter;
  dsoundbuffer = GST_DIRECTSOUND_RING_BUFFER (buf);

  do {
    GST_DSOUND_LOCK (dsoundbuffer);

    if (dsoundbuffer->flushing || !dsoundbuffer->pDSCB8) {
      GST_DSOUND_UNLOCK (dsoundbuffer);
      goto complete;
    }

    GST_DSOUND_UNLOCK (dsoundbuffer);

  restore_buffer:
    /* get current buffer status */
    GST_DSOUND_LOCK (dsoundbuffer);
    hr = IDirectSoundCaptureBuffer8_GetStatus (dsoundbuffer->pDSCB8, &dwStatus);
    GST_DSOUND_UNLOCK (dsoundbuffer);

    // ###: the capture api doesn't seem to have _BUFFERLOST and _Restore,
    //   so commenting this out until it can be investigated.
#if 0
    if (dwStatus & DSBSTATUS_BUFFERLOST) {
      GST_DEBUG ("Buffer was lost, attempting to restore");

      GST_DSOUND_LOCK (dsoundbuffer);
      hr = IDirectSoundBuffer8_Restore (dsoundbuffer->pDSB8);
      GST_DSOUND_UNLOCK (dsoundbuffer);

      /* restore may fail again, ensure we restore the
       * buffer before we continue */
      if (FAILED(hr) && hr == DSERR_BUFFERLOST) {
        if (retries++ < MAX_LOST_RETRIES) {
          GST_DEBUG ("Unable to restore, trying again");
          goto restore_buffer;
        }
        else {
          GST_ELEMENT_ERROR (dsoundbuffer->element, RESOURCE, FAILED,
             ("%ls.", DXGetErrorDescription9W(hr)),
             ("gst_directsound_read_proc: IDirectSoundBuffer8_Restore, hr = %X", (unsigned int) hr));
          goto complete;
        }
      }
    }
#endif

    /* Starting capturing if not already */
    if (!(dwStatus & DSCBSTATUS_CAPTURING)) {
      hr = IDirectSoundCaptureBuffer8_Start (dsoundbuffer->pDSCB8,
          DSCBSTART_LOOPING);
      if (FAILED(hr)) {
        GST_WARNING ("gst_directsound_read_proc: IDirectSoundCaptureBuffer8_Start, hr = %X", (unsigned int) hr);
      }
    }

    /* get current capture cursor and read cursor positions */
    GST_DSOUND_LOCK (dsoundbuffer);
    hr = IDirectSoundCaptureBuffer8_GetCurrentPosition (dsoundbuffer->pDSCB8,
        &dwCurrentCaptureCursor, &dwCurrentReadCursor);
    GST_DSOUND_UNLOCK (dsoundbuffer);

    if (G_UNLIKELY (FAILED(hr))) {
      /* try and reopen the default directsound device */
      if (hr == DIRECTSOUND_ERROR_DEVICE_RECONFIGURED) {
        /* we have to wait a while for the sound device removal to actually
         * be processed before attempting to reopen the device. Yes, this
         * sucks */
        Sleep (2000);

        GST_DSOUND_LOCK (dsoundbuffer);
        IDirectSoundCaptureBuffer8_Release (dsoundbuffer->pDSCB8);
        dsoundbuffer->pDSCB8 = NULL;
        GST_DSOUND_UNLOCK (dsoundbuffer);

        if (gst_directsound_ring_buffer_close_device (buf) &&
            gst_directsound_ring_buffer_open_device (buf) &&
            gst_directsound_create_buffer (buf) ) {
          dsoundbuffer->buffer_circular_offset = 0;
          goto restore_buffer;
        }
      }

      /* only trigger an error if we're not already in an error state */
      if (FAILED(hr) && !error) {
        GST_ELEMENT_ERROR (dsoundbuffer->element, RESOURCE, FAILED,
           ("%ls.", DXGetErrorDescription9W(hr)),
           ("gst_directsound_read_proc: IDirectSoundCaptureBuffer8_GetCurrentPosition, hr = %X", (unsigned int) hr));
        error = TRUE;
        goto complete;
      }
    }

    GST_LOG ("Current Read Start: %u Current Read End: %d",
        dsoundbuffer->buffer_circular_offset,
        (unsigned int) dwCurrentReadCursor);

    /* calculate the captured amount in the circular buffer */
    GST_DSOUND_LOCK (dsoundbuffer);
    if (dwCurrentReadCursor >= dsoundbuffer->buffer_circular_offset)
      capturedBufferSize = dwCurrentReadCursor -
          dsoundbuffer->buffer_circular_offset;
    else
      capturedBufferSize = dsoundbuffer->buffer_size -
        (dsoundbuffer->buffer_circular_offset - dwCurrentReadCursor);
    GST_DSOUND_UNLOCK (dsoundbuffer);

    if (!gst_ring_buffer_prepare_read (buf, &writeseg, &writeptr, &len))
      goto complete;

    len -= dsoundbuffer->segoffset;

    if (len > capturedBufferSize)
      len = capturedBufferSize;

    GST_LOG ("Size of segment to read: %d Captured buffer size: %lld",
        len, capturedBufferSize);

    /* If we can't read from directsound because we don't have enough
     * captured data, then sleep for a little while to wait until data is
     * available */
    // ###: why >= and not > ?
    // ###: what happens if this condition is true on the first iteration?
    //   capture totally busted?
    //if (len >= capturedBufferSize) {
    //  goto complete;
    //}
    if (len <= 0)
      goto complete;

    /* lock it */
    GST_DSOUND_LOCK (dsoundbuffer);
    hr = IDirectSoundCaptureBuffer8_Lock (dsoundbuffer->pDSCB8,
        dsoundbuffer->buffer_circular_offset, len, &pLockedBuffer1,
        &dwSizeBuffer1, &pLockedBuffer2, &dwSizeBuffer2, 0L);

    /* copy chunks */
    if (SUCCEEDED (hr)) {
      if (len <= dwSizeBuffer1) {
        memcpy ((LPBYTE) writeptr + dsoundbuffer->segoffset, pLockedBuffer1, len);
      }
      else {
        memcpy ((LPBYTE) writeptr + dsoundbuffer->segoffset, pLockedBuffer1, dwSizeBuffer1);
        memcpy ((LPBYTE) writeptr + dsoundbuffer->segoffset + dwSizeBuffer1, pLockedBuffer2, len - dwSizeBuffer1);
      }

      IDirectSoundCaptureBuffer8_Unlock (dsoundbuffer->pDSCB8, pLockedBuffer1,
         dwSizeBuffer1, pLockedBuffer2, dwSizeBuffer2);
    }
    else {
      GST_WARNING ("gst_directsound_read_proc: IDirectSoundCaptureBuffer8_Lock, hr = %X", (unsigned int) hr);
    }

    /* update tracking data */
    dsoundbuffer->segoffset += len;

    dsoundbuffer->buffer_circular_offset += len;
    dsoundbuffer->buffer_circular_offset %= dsoundbuffer->buffer_size;
    GST_DSOUND_UNLOCK (dsoundbuffer);

    capturedBufferSize -= len;

    GST_LOG ("DirectSound Buffer1 Data Size: %u DirectSound Buffer2 Data Size: %u",
        (unsigned int) dwSizeBuffer1, (unsigned int) dwSizeBuffer2);
    GST_LOG ("Captured buffer size remaining: %lld", capturedBufferSize);

    /* check if we wrote a whole segment */
    GST_DSOUND_LOCK (dsoundbuffer);
    if (dsoundbuffer->segoffset == dsoundbuffer->segsize) {
      GST_DSOUND_UNLOCK (dsoundbuffer);

      /* advance to next segment */
      gst_ring_buffer_advance (buf, 1);

      GST_DSOUND_LOCK (dsoundbuffer);
      dsoundbuffer->segoffset = 0;
    }
    GST_DSOUND_UNLOCK (dsoundbuffer);

  complete:
    GST_DSOUND_LOCK (dsoundbuffer);

    should_run = dsoundbuffer->should_run;
    flushing = dsoundbuffer->flushing;
    retries = 0;

    GST_DSOUND_UNLOCK (dsoundbuffer);

    /* it's extremely important to sleep in without the lock! */
    if (len >= capturedBufferSize || flushing || error)
      Sleep (dsoundbuffer->min_sleep_time);
  }
  while(should_run);

  return 0;
}
