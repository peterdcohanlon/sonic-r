# How the Dreamcast music pause works

When the game is paused, the music stops. When it's resumed, the music carries
on from **the exact sample it stopped on**. On the desktop builds that's one
call to SDL_mixer (`Mix_PauseMusic` / `Mix_ResumeMusic`). On the Dreamcast it
takes more work, because KallistiOS has no way to pause a music stream. This
document explains exactly how it's done.

Files involved:

| File | Role |
|---|---|
| `source/sdl/src/input/input.c` | Calls `PauseCD()` when the pause menu opens, `ResumeCD()` when you pick Resume |
| `source/dc/src/music_dc.c` | `PauseCD()` / `ResumeCD()`, and keeps a paused track counted as "still playing" |
| `source/dc/src/sndwav.c` | `.adp` music backend (decoded by the sound chip) |
| `source/dc/src/sndadx.c` | `.adx` music backend (decoded by the SH4 CPU) |
| `source/dc/src/snd_resume.c` / `.h` | The exact-resume logic shared by both backends |

---

## 1. Why KOS can't simply pause

Music is streamed with KOS's `snd_stream` API (`kernel/arch/dreamcast/sound/snd_stream.c`).
It works like this:

- **The ring.** Each audio channel (left and right) gets a fixed-size **ring buffer** in
  the sound chip's memory (the AICA's RAM). The AICA plays the ring in a loop,
  start to end, then wraps around.
- **Prefill.** `snd_stream_start()` fills the whole ring from our data callback, sets
  its write position to 0, and starts the AICA channels at position 0.
- **Refilling.** `snd_stream_poll()`, called by our audio thread every 20 ms, reads where the
  AICA is currently playing and asks our callback for more data to fill the
  space between its **write position** and the **play position**. So the ring
  is kept nearly full: almost everything in it is data that has been handed
  over but **not played yet**.
- **Stopping.** `snd_stream_stop()` just halts the AICA channels. There is no "pause",
  and on the next `snd_stream_start()` the ring is prefilled again from the
  callback, overwriting it.

So the old pause (stop, then start) threw away everything that was queued in
the ring but hadn't been heard. The music jumped forward by up to a full ring:

| Backend | Ring size per channel | Samples per channel | Time at 44.1 kHz |
|---|---|---|---|
| `.adp` (4-bit ADPCM) | 8,192 bytes | 16,384 | ~0.37 s |
| `.adx` (decoded to 16-bit PCM) | 32,768 bytes | 16,384 | ~0.37 s |

That ~0.37 s is the jump you'd hear on every resume.

## 2. The idea

If we know **which sample was playing** at the moment of the pause, and we've
kept a copy of **everything we've handed to KOS**, then on resume we can hand
KOS the unheard part again, before carrying on with new data. The AICA restarts
on exactly the sample it stopped at.

That needs three things:

1. A **history** of the bytes the callback has delivered.
2. The **play position** at the moment of the pause.
3. The **write position**, so we know how much of the ring was unplayed.

## 3. The history

`snd_resume_t` holds a circular history buffer, sized to **two full stereo
rings** (32 KB for `.adp`, 128 KB for `.adx`):

- **`SndResume_Record()`** copies every freshly read or decoded block into the history
  as the callback produces it.
- **`SndResume_Take()`** serves bytes back out of the history when there's a replay
  pending.
- **`behind`** counts how many bytes are waiting to be replayed.

The callback in each backend now goes:

```
1. Take() as much pending replay as fits in this request
2. if that wasn't enough: read/decode the rest from the file as before,
   and Record() the new bytes into the history
3. Delivered() the total, so we keep count of what KOS has received
```

Replayed bytes are **not** recorded a second time, so the history always holds
the music in order, and "go back N bytes" means exactly N bytes back in the
song.

The history is twice the size a single pause needs. That way, if you pause
again before an earlier replay has finished, the second rewind still fits.

## 4. Where is the sound chip playing?

KOS exposes `snd_get_pos(channel)`, which reads the AICA's current play
position for a channel (in samples, within the ring). But it needs the
**channel number**, and KOS doesn't say which channel a stream got.

`snd_stream_alloc()` takes its first channel with `snd_sfx_chn_alloc()`, which
always returns the **lowest free channel**. So, right before the stream is
allocated, `SndResume_Init()` claims a channel and immediately frees it again.
Nothing allocates in between, so that's the channel the stream is about to
get. Sound effects don't take channels this way (they pick from the unreserved
ones), so the answer holds for the life of the stream.

## 5. Where is KOS writing?

KOS keeps its write position (`last_write_pos`) private, but it can be worked
out exactly from what we've delivered:

- After `snd_stream_start()` the ring has been prefilled whole and the write
  position is 0.
- Every refill writes strictly in order, advancing the write position by
  exactly the number of samples we delivered.

So `SndResume_Delivered()` keeps a count of **samples per channel delivered
since the last start** (`written`), and:

```
write position = written mod ring_samples
```

Converting bytes to samples per channel:

- **ADPCM** (4-bit): 1 byte per channel = 2 samples. In stereo, each delivered byte
  holds one left and one right sample (KOS splits them by nibble), so
  `samples per channel = bytes delivered`.
- **PCM16**: 2 bytes per sample, so in stereo `samples per channel = bytes / 4`.

## 6. The pause: `SndResume_MarkPause()`

When the game pauses, `PauseCD()` → `wav_pause()` → `SndResume_MarkPause()`:

```
play     = snd_get_pos(ch0)                       // AICA play position (in the ring)
written  = samples delivered since start
write    = written mod ring_samples               // KOS write position
unplayed = (write − play) mod ring_samples        // queued but not heard
if unplayed == 0: unplayed = ring_samples         // write == play means "full", not "empty"
pause_at = written − unplayed                     // absolute sample that was playing
```

`pause_at` is the key number. It's the position in the song, counted from the
stream's start, of the sample that was coming out of the speaker at that
instant.

The order of the two reads matters. The play position is read **first**. If a
refill sneaks in before `written` is read, it only adds data *ahead* of the
play head. That raises `write` and `unplayed` by the same amount, so `pause_at`
comes out the same.

### Why the pause call takes no lock

This was the subtle part, and an independent review confirmed the reasoning.

- The VMU save code (`save_vmu.c`) calls `PauseCD()` **while holding `io_lock`**,
  the global file I/O lock, to keep the music quiet while it uses the maple bus.
- Meanwhile, the audio thread holds the stream mutex while it runs the refill
  callback, and that callback reads the music file, which takes `io_lock`.

If the pause took the stream mutex, a save could deadlock the game: the
main thread would hold `io_lock` and want the mutex, while the audio thread
held the mutex and wanted `io_lock`. So `MarkPause()` only reads two values
(a hardware register and a counter) and writes three of its own. It mutes the
stream and hands the actual stop to the audio thread, as the original code
did.

## 7. The stop: `SndResume_OnStopped()`

On its next tick (within 20 ms) the audio thread sees the pause request, calls
`snd_stream_stop()`, then `SndResume_OnStopped()`:

```
rewind_samples = written − pause_at     // everything delivered from the paused sample on
rewind_bytes   = samples → bytes (all channels)
behind         = rewind_bytes + behind  // plus any replay that was still pending
behind         = min(behind, bytes held in history)
```

That covers what was queued at the pause **and** anything KOS topped up while
the stop was pending (the stream was muted, but still playing for those few
milliseconds), so none of it is lost.

## 8. The resume

`ResumeCD()` → `wav_play()` asks the audio thread to restart. It runs
`SndResume_OnStart()` (resetting `written` to 0) and then `snd_stream_start()`.
The start prefills the ring from the callback, and the callback's first job is
to `Take()` the pending replay. So the ring begins with **the sample that was
playing when you paused**, and the AICA starts playing from there.

Two edge cases are handled:

- **A very quick pause/resume:** if `ResumeCD()` arrives before the audio thread has
  handled the stop, the thread finishes the stop and rewind first, then starts.
- **Unmuting too early:** `wav_play()` doesn't unmute while a pause is still waiting
  to be handled. Otherwise you'd briefly hear the old stream running past the
  pause point before the rewind.

## 9. Keeping the game from restarting the track

While the game is paused, the race loop still runs `UpdateGameLogic()` every
frame, and that keeps asking the music system "is the track still playing?"
(`GetLogicalCDTrack()`). A stopped stream reports "not playing", so the game
would conclude the track had ended and start it again **from the beginning**.

`music_dc.c` now treats a paused track as still being the current track:

- `GetLogicalCDTrack()` returns the current track while `s_paused` is set.
- `play_track()` won't restart the current track while it's paused.

## 10. A worked example (`.adp`, stereo)

| | Value |
|---|---|
| Ring | 8,192 bytes per channel = 16,384 samples |
| Delivered since start (`written`) | 100,000 samples |
| Write position | 100,000 mod 16,384 = **1,696** |
| AICA play position at pause (`play`) | **4,000** |
| Unplayed | (1,696 − 4,000) mod 16,384 = **14,080** samples (~0.32 s) |
| `pause_at` | 100,000 − 14,080 = **85,920** |

Then, before the audio thread stops the stream, one more refill delivers 2,048
samples, so `written` = 102,048:

| | Value |
|---|---|
| Rewind | 102,048 − 85,920 = **16,128** samples |
| In bytes (stereo ADPCM: 1 byte per sample-frame) | **16,128 bytes** |

On resume, the first 16,128 bytes handed to KOS come from the history. The
first of them is sample 85,920, the one that was playing when you paused.

## 11. What "exact" means for each format

- **`.adx` (PCM16):** exact to the sample. The CPU has already decoded the audio, so
  replaying the bytes reproduces the waveform precisely.
- **`.adp` (ADPCM):** the right data, resumed at the right sample. However, the AICA
  decodes ADPCM in hardware and resets its decoder state when a channel starts,
  so resuming in the middle of a stream can cause a tiny click. That was
  equally true of the old stop/start (and of the existing pause around slow
  disc loads); it's a property of the hardware decoder, not of this change.

## 12. Side benefits

The same `PauseCD()` / `ResumeCD()` pair was already used in two places: around
big loads on slow media (burned CD-Rs), and around VMU access. Both now resume
without skipping ahead too.

There's also a smaller win. When the VMU code resumes the music while still
holding `io_lock`, the restart's prefill is now served mostly from the history
instead of the file. So it no longer waits on `io_lock` for the full 2.5 s
timeout that `wav_play()` allows.

## 13. Testing

- **Real Dreamcast with GDEMU:** pause and resume mid-race, with both `.adx` and `.adp` music.
- **Flycast:** `.adx` music, pause and resume.
- **Review:** an independent review checked the position arithmetic against KOS's
  `snd_stream.c` fill and poll code, the ADPCM stereo layout in
  `snd_pcm_split.s`, and `snd_sfx_chn_alloc()`.
