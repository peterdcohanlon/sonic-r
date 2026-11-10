/* snd_resume.c — sample-exact pause/resume for a KOS snd_stream.
 * See snd_resume.h. */

#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <stdlib.h>
#include <string.h>

#include "snd_resume.h"

int SndResume_Init(snd_resume_t *r, size_t ring_bytes_per_channel)
{
    memset(r, 0, sizeof(*r));
    r->ch0 = -1;
    r->ring_bytes = ring_bytes_per_channel;
    /* One full stereo ring is the most a single pause can need; double it so
     * a second pause that lands while an earlier replay is still pending
     * stays exact too. */
    r->size = ring_bytes_per_channel * 4;
    r->hist = malloc(r->size);
    if (!r->hist) {
        r->size = 0;
        return -1;
    }

    /* snd_stream_alloc() takes its first channel with snd_sfx_chn_alloc(),
     * which hands out the lowest free channel. Claiming and releasing one
     * here, with nothing allocating in between, names the same channel.
     * Sound effects never take channels this way — they pick from those left
     * unreserved — so the answer holds for the life of the stream. */
    int ch = snd_sfx_chn_alloc();
    if (ch >= 0) {
        snd_sfx_chn_free(ch);
    }
    r->ch0 = ch;
    return 0;
}

void SndResume_Reset(snd_resume_t *r)
{
    r->head = 0;
    r->filled = 0;
    r->behind = 0;
    r->written = 0;
    r->pause_marked = 0;
}

void SndResume_OnStart(snd_resume_t *r, int bits, int channels)
{
    r->bits = bits;
    r->channels = (channels == 2) ? 2 : 1;
    r->written = 0;
    r->pause_marked = 0;
}

/* Samples per channel <-> bytes as handed to the callback (all channels). */
static uint32_t bytes_to_samples(const snd_resume_t *r, size_t bytes)
{
    size_t per_ch = bytes / (size_t)r->channels;
    return (uint32_t)((r->bits == 4) ? per_ch * 2 : per_ch / 2);
}

static size_t samples_to_bytes(const snd_resume_t *r, uint32_t samples)
{
    size_t per_ch = (r->bits == 4) ? samples / 2 : (size_t)samples * 2;
    return per_ch * (size_t)r->channels;
}

size_t SndResume_Take(snd_resume_t *r, uint8_t *dst, size_t want)
{
    if (r->behind == 0 || r->size == 0) {
        return 0;
    }
    size_t n = (r->behind < want) ? r->behind : want;
    /* Start of the replay region: `behind` bytes back from the write head. */
    size_t pos = (r->head + r->size - r->behind) % r->size;
    size_t first = r->size - pos;
    if (first > n) {
        first = n;
    }
    memcpy(dst, r->hist + pos, first);
    memcpy(dst + first, r->hist, n - first);
    r->behind -= n;
    return n;
}

void SndResume_Record(snd_resume_t *r, const uint8_t *src, size_t n)
{
    if (r->size == 0) {
        return;
    }
    if (n >= r->size) {
        src += n - r->size;
        n = r->size;
    }
    size_t first = r->size - r->head;
    if (first > n) {
        first = n;
    }
    memcpy(r->hist + r->head, src, first);
    memcpy(r->hist, src + first, n - first);
    r->head = (r->head + n) % r->size;
    r->filled += n;
    if (r->filled > r->size) {
        r->filled = r->size;
    }
}

void SndResume_Delivered(snd_resume_t *r, size_t n)
{
    if (r->channels == 0) {
        return;
    }
    r->written += bytes_to_samples(r, n);
}

void SndResume_MarkPause(snd_resume_t *r)
{
    r->pause_marked = 0;
    if (r->size == 0 || r->ch0 < 0 || r->channels == 0) {
        return;
    }

    uint32_t ring = (r->bits == 4) ? (uint32_t)(r->ring_bytes * 2)
                                   : (uint32_t)(r->ring_bytes / 2);

    /* Play position first, then the delivered count. A refill landing in
     * between only adds data ahead of the play head, which leaves the result
     * below unchanged. */
    uint32_t play = snd_get_pos((unsigned int)r->ch0);
    uint32_t written = r->written;
    if (play >= ring || written == 0) {
        return;
    }

    /* Queued but not yet heard. KOS keeps its write head behind the play
     * head, so write == play means the ring is full (just after a start). */
    uint32_t unplayed = (written % ring + ring - play) % ring;
    if (unplayed == 0) {
        unplayed = ring;
    }
    if (unplayed > written) {
        unplayed = written;
    }
    r->pause_at = written - unplayed;
    r->pause_marked = 1;
}

void SndResume_OnStopped(snd_resume_t *r)
{
    if (!r->pause_marked) {
        return;
    }
    r->pause_marked = 0;

    /* Everything delivered from the marked sample on — what was queued then,
     * plus anything topped up while the stop was pending — goes out again.
     * Bytes still waiting from an earlier, unfinished replay come after it in
     * the stream, so they stay queued as well. */
    size_t rewind = samples_to_bytes(r, r->written - r->pause_at) + r->behind;
    if (rewind > r->filled) {
        rewind = r->filled;
    }
    r->behind = rewind;
}
