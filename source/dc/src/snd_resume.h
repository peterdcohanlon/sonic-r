/* snd_resume.h — sample-exact pause/resume for a KOS snd_stream.
 *
 * KOS streams have no pause: snd_stream_stop() discards whatever is already
 * queued in the AICA ring, and snd_stream_start() refills it from the data
 * callback. Stop + start therefore skips ahead by however much of the ring
 * had not played yet — up to a whole ring (~0.37 s of music).
 *
 * This keeps a copy of the most recent bytes the callback handed to KOS. When
 * a pause is requested it notes exactly which sample was playing; once the
 * stream has actually been stopped it works out how much of what was handed
 * over never got heard, and the next start feeds exactly those bytes again
 * before going back to the decoder/file. Playback resumes on the sample where
 * the pause was requested.
 *
 * Positions are kept as an absolute count of samples (per channel) delivered
 * since the last start. KOS's ring write position isn't exposed, but it is
 * that count modulo the ring length: the ring is prefilled whole, then topped
 * up strictly in order.
 *
 * Threading: SndResume_MarkPause() takes no lock, because the game calls
 * PauseCD() from the VMU code while holding io_lock, and the audio thread can
 * be holding the stream mutex while waiting on io_lock inside the callback.
 * Everything else runs under the backend's stream mutex (the callback and the
 * audio thread already do).
 */
#ifndef SND_RESUME_H
#define SND_RESUME_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *hist;              /* last `size` bytes delivered to KOS */
    size_t   size;              /* = two full stereo rings */
    size_t   head;              /* next write index into hist */
    size_t   filled;            /* valid bytes in hist (<= size) */
    size_t   behind;            /* bytes to replay from hist before new data */
    int      ch0;               /* AICA channel carrying the left/mono stream */
    size_t   ring_bytes;        /* KOS ring bytes per channel (the alloc size) */
    int      bits;              /* 4 = ADPCM, 16 = PCM16 (set at start) */
    int      channels;          /* 1 or 2 (set at start) */
    volatile uint32_t written;  /* samples per channel delivered since start */
    volatile uint32_t pause_at; /* absolute sample that was playing at pause */
    volatile int      pause_marked;
} snd_resume_t;

/* Allocate the history. Call right before snd_stream_alloc(), which it uses
 * to learn the AICA channel the stream will be given. Returns 0 on success;
 * on failure pause/resume falls back to the old skip-ahead behaviour. */
int  SndResume_Init(snd_resume_t *r, size_t ring_bytes_per_channel);

/* New track or full stop: nothing from before is worth replaying. */
void SndResume_Reset(snd_resume_t *r);

/* Call immediately before snd_stream_start*(). */
void SndResume_OnStart(snd_resume_t *r, int bits, int channels);

/* Callback helpers. Take() fills dst with pending replay bytes and returns
 * how many it wrote; Record() appends freshly produced bytes to the history;
 * Delivered() is told the total handed to KOS by this callback. */
size_t SndResume_Take(snd_resume_t *r, uint8_t *dst, size_t want);
void   SndResume_Record(snd_resume_t *r, const uint8_t *src, size_t n);
void   SndResume_Delivered(snd_resume_t *r, size_t n);

/* Pause requested while streaming: note the sample playing now. Lock-free. */
void SndResume_MarkPause(snd_resume_t *r);

/* The stream has just been stopped for that pause: queue for replay
 * everything delivered from the marked sample on. */
void SndResume_OnStopped(snd_resume_t *r);

#endif
