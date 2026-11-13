# bear

An interactive, spring-driven black-orb pet for ESP32-P4. `bear` is its name,
not its silhouette: the visual remains a minimal black blob with two white
polygon eyes. The renderer uses the quality tier selected by the on-device
vector benchmark: a 96-vertex body, palette-interpolated anti-aliasing and two
HostSurface buffers.

## Interaction

- Touch outside bear to enter listening mode and look toward the contact.
- Drag to make bear curious, lean and follow the pointer.
- Touch the upper half to make bear excited and bounce.
- Touch the lower half for a sideways wobble; touch either eye to wink it.
- Release to let every part settle naturally back into the idle pose.
- Hold the awake, idle bear for five seconds to open settings; moving more than
  20 pixels or releasing early cancels the hold.
- When left alone, bear breathes, drifts and blinks, then becomes drowsy and
  falls asleep. The first touch while drowsy or asleep only wakes it.

## Sound

When audio output is available, touch interactions use quiet synthesized cues:
listening when a voice session starts, curiosity when a drag first makes bear
curious (with the curious reaction enabled), and excitement when a tap or wink
first makes bear excited. A cue plays only when the touch actually changes
bear's mood, so holding for settings and cancelling voice stay silent. The
"触摸音效" row in Settings -> Bear 行为 disables all of these cues.

Waking from drowsiness or sleep uses a separate cue, "首次唤醒提示音" in
Settings -> 声音与语音. It plays only on that wake transition; touching an
already awake bear never replays it. Drowsiness, sleep and settings navigation
are silent. Visual interaction remains fully functional on devices without
audio.

Bear does not display transcript or response text. Response streaming is a Host
capability with no Guest-side switch, so Settings -> 声音与语音 marks it as
Host-managed rather than offering a toggle.

Build and run on an attached board:

```sh
python3 tools/micropixel --transport usb run guest/apps/bear \
    --profile performance
```

The character geometry and palette are original to this application. The
motion design uses independent damped springs for pose, gaze, expression and
blink state.
