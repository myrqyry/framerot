# Using framerot

[Back to the README](../README.md) · [Technical reference](technical.md) · [Build and validation](development.md)

## Install on Linux

The native Linux x86_64 package is built against OBS 32.2.0 on Ubuntu 24.04 and uses system FFmpeg 6 libraries. A compatible OBS runtime and libraries are required (the build uses libavcodec.so.60, libavutil.so.58 and libswscale.so.7). Windows, macOS and Flatpak installation are not validated.

Download the Linux artifact from a successful [Build and test run](https://github.com/myrqyry/framerot/actions) for the revision you want. Unzip the GitHub artifact to find the tarball, then close OBS and extract it into your user plugin directory:

~~~bash
mkdir -p "$HOME/.config/obs-studio/plugins"
tar -xzf framerot-linux-x86_64-obs32.tar.gz -C "$HOME/.config/obs-studio/plugins"
~~~

Run the commands from the directory containing the tarball. The installed module should be at:

~~~text
~/.config/obs-studio/plugins/framerot/bin/64bit/framerot.so
~~~

Reopen OBS. Right-click a video source and select **Filters**, then use the **+** under **Effect Filters** to add **framerot**. Scroll to the bottom if needed. It is a video filter, not an input source.

To remove it, close OBS and move the framerot plugin folder out of the plugin search directory. Existing scenes that refer to the filter may require attention when it is absent.

## Your first datamosh

Start with a webcam or a video containing moving objects and hard cuts. A static image may show compression but little temporal decay. Add framerot and let the default Instrument combination run for a few seconds.

Try reducing Wet mix to 50%, then move it back to 100%. Change the horizontal motion delta while moving the camera. Hold a reference, move around, and release it to see a fresh image. Switch on Frame Peel to layer in horizontal bands from earlier decoded frames.

Use **Reset codec and history** whenever you want a fresh start without losing your configuration. OBS's **Defaults** button instead restores the filter's default settings.

## Instrument controls

Instrument combines two packet-operation selectors and four independent layers. The interface only shows relevant sliders. Hidden values are retained, so turning a layer back on restores your previous adjustments.

### Reference frames

| Choice | Behavior | Controls |
| --- | --- | --- |
| Live | Let keyframes refresh normally. | — |
| Skip keyframes | Skip selected new keyframes so current motion draws on older references. | Skipped keyframes (0–8). |
| Cycle anchor | Replay an earlier compressed keyframe for several keyframe intervals. | Anchor span (2–12). |

### Predictive frames

| Choice | Behavior | Controls |
| --- | --- | --- |
| Live | Keep incoming motion packets. | — |
| Tear | Drop every Nth predictive packet. | P-frame interval. |
| Burst | Drop several predictive packets in each interval. | P-frame interval, burst length. |
| Echo | Periodically replay an earlier compressed packet. | P-frame interval. |
| Rewind | Substitute packets from a fixed number of predictive frames ago. | Rewind depth (2–12). |

Interval is 2–60 frames; burst length is 1–30 but always constrained below the interval. Rewind history resets at keyframes, so a longer keyframe interval is useful when using deeper rewind.

### Layers

| Layer | Controls | Creative use |
| --- | --- | --- |
| Packet editing | Motion delta X/Y (−500–500%), residual strength (−200–200%), block loss (0–100%). | 100% motion and residual strength are neutral; try negative motion for reversed deltas or lower residual strength for lingering detail. |
| Flow Drift | Motion exaggeration (0–500%), persistence (0–100%). | Raise persistence for more previous-frame feedback. Zero in either control disables the Flow contribution. |
| Frame Peel | History (2–8 frames), band height (1–64 codec pixels). | Smaller bands create finer strips; larger history samples older frames. |
| Motion Shimmer | Edit every N P-frames (1–60). | A comparatively subtle change in how the codec interpolates motion. |

Packet editing works on actual encoded motion **deltas**, not an absolute optical-flow field. Neutral packet editing is X/Y 100%, residual strength 100%, block loss 0%.

### Common settings

| Setting | Range | What it changes |
| --- | --- | --- |
| Codec height | 64–576 px, steps of 16 | Internal codec resolution; lower tends toward chunky artifacts. |
| Codec frame rate | 1–30 FPS | How frequently new codec frames are produced, independent of OBS FPS. |
| Bitrate | 8–2000 kb/s | Compression budget; lower generally increases compression damage. |
| Keyframe interval | 2–120 frames | How frequently the encoder produces a full reference frame. |
| Wet mix | 0–100% | Original image to fully processed image. |
| Preserve live source alpha | On/off | Use the current source's transparency instead of opaque codec RGB. |

Codec dimensions are internal only; the output still fills the source. At 100% wet, disabling alpha preservation can reduce full-size capture work between codec frames; only use this on sources where losing transparency is acceptable.

### Default Instrument settings

| Setting | Default |
| --- | --- |
| Reference / predictive | Cycle anchor / Live |
| Anchor span | 3 |
| Layers | Packet editing and Flow Drift on; Frame Peel and Motion Shimmer off |
| Motion delta X / Y | 160% / 100% |
| Residual strength / loss | 70% / 10% |
| Flow exaggeration / persistence | 150% / 40% |
| Codec height / FPS | 192 px / 18 |
| Bitrate / keyframe interval | 320 kb/s / 18 frames |
| Wet mix / preserve alpha | 100% / on |

Explicit saved values take precedence over defaults when changing modes or reopening a scene. Older filters with implicit defaults may inherit new defaults. Starting points intentionally overwrite the effect and codec values; wet mix and alpha are preserved.

## Creative starting points

Choose these under **Instrument → Load starting point**. They are suggestions, not guaranteed looks; try them on your own footage.

- **Memory Melt:** Cycle anchor, live predictive packets and packet editing. 288 px / 30 FPS / 650 kb/s. Hold a reference during a movement or hard cut, then try reversing the horizontal motion delta.
- **Motion Smear:** Live reference and predictive packets, packet editing and Flow Drift. 192 px / 15 FPS / 250 kb/s. Try this on a moving subject, then tune residual strength and flow persistence.
- **Time Ribbons:** Live reference, Echo and Frame Peel. 144 px / 15 FPS / 180 kb/s. Change band height and history depth for different temporal patterns.
- **Randomize (experimental):** Picks one or two compatible operations and bounded values. The generated settings are editable and saved normally. Select it again to roll again. It does not animate settings. See [technical bounds](technical.md#instrument-mode).

The picker returns to Custom after applying any starting point. This is intentional: your current settings are the result, not a continuously enforced preset.

## Live controls

**Hold reference** latches the last admitted or captured compressed I-frame for reuse at subsequent keyframe boundaries, rather than freezing the output immediately. Current predictive motion continues. **Release reference** forces a fresh I-frame on the next processed input. **Reset codec and history** starts a new codec generation and releases the hold.

Find the filter's hold-reference and reset actions in **Settings → Hotkeys**. Hold is momentary while the bound key is pressed; the properties button is latched until released. Physical hotkey bindings have not yet been qualified in the desktop test.

## Historical compression textures

For authentic H.263, Cinepak, Sorenson, Microsoft Video 1, MPEG-1 and other era-inspired codec profiles, use [Retro Effects](https://github.com/myrqyry/obs-retro-effects) with its FFmpeg codec gallery enabled. Historical profile, full-frame presentation, wet mix and live alpha are available in the updated fork. framerot no longer includes a separate Codec Crush mode.

Existing saved datamosh mode IDs are unchanged. If an experimental old filter has an unavailable effect selected, it falls back to Instrument; use Retro Effects instead for historical compression.

## Troubleshooting

| Symptom | Try |
| --- | --- |
| Cannot find framerot | Restart OBS after installation; look near the bottom of Effect Filters (not Sources). Verify the module path above and look for framerot in **Help → Log Files → View Current Log**. |
| Module fails to load | Check that the binary matches your architecture, OBS and system FFmpeg libraries. On native Linux, run the dependency check below. |
| Effect seems subtle | Use moving footage or hard cuts; raise wet mix, try Memory Melt or Classic, or hold a reference while moving. |
| Image has become too chaotic | Lower wet mix, release the reference or reset codec and history. Reduce motion exaggeration or block loss. |
| Render lag or stutters | Lower codec height and FPS, disable expensive layers. GPU readback can still stall. |
| Transparent source becomes opaque | Enable Preserve live source alpha. The codec does not edit alpha itself. |
| HDR output appears unchanged | The SDR codec path bypasses HDR framebuffers. |

~~~bash
ldd "$HOME/.config/obs-studio/plugins/framerot/bin/64bit/framerot.so"
~~~

Check for missing dependencies rather than copying or symlinking incompatible FFmpeg versions into the plugin directory. If your runtime is incompatible, use a matching build or build against your own compatible OBS/FFmpeg SDK.

framerot is experimental. Test unfamiliar combinations in a disposable scene before your live broadcast. If reporting a problem, include the OBS version, OS, GPU, filter settings, relevant OBS log lines and steps to reproduce (omit private paths or other sensitive details).
