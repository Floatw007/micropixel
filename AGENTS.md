# Repository instructions

Applies to the entire repository. Use source code, ABI headers, and executable tests as the authority for behavior.

## Architecture

MicroPixel runs WebAssembly apps on Espressif MCUs using ESP-IDF 6.1 and a pinned WAMR fork (AOT v6).
Guests use a restricted C++23 SDK and a single-threaded event loop; only one AppSession can exist at a time.
Supported boards are Metalio-Claw4, ESP-Mosaico, ESP32-S3-BOX-3, LCKFB SZPI, and M5Stack CoreS3.
Bundle v1 distributes apps; BundleFS v3 provides copy-on-write storage and power-loss recovery with
geometry derived from the block device (v1/v2 catalogs are imported in place).

Dependencies: `Runtime → Device contracts ← Platform`. `FirmwareApp` is the only composition root.
The Host owns hardware, app lifecycle, and system UI. Guests access capabilities through the Service ABI.

## Bear product objective

The user-defined end goal is an interactive multimedia platform on a wheel-legged robot, including the ability
to control robot movement and coordinate Bear's interactions with the wheel-legged chassis. The on-screen pet,
touch interaction, voice conversation and audio/visual presentation are foundations of this platform, not its
entire final scope.

Track progress against the complete product scope:

- Interactive multimedia: Bear's visual character, touch, voice, audio and settings.
- Robot integration: a defined chassis communication interface, motion commands and robot-state feedback.
- Coordinated behavior: interactions between Bear and chassis movement, with corresponding multimedia feedback.
- Integrated validation: verify the agreed interactions and motion-control behavior on the actual robot, including
 interruption, communication loss and fault handling.

The first paragraph is the confirmed product goal. The categories above organize future planning; they do not
establish a finalized command set or implementation design. Chassis hardware, transport/protocol, division of
control responsibility, supported motions, specific coordinated behaviors and measurable acceptance criteria
remain to be agreed and documented. Do not assume that Bear implements low-level balance or motor control.
Follow the existing Host/Guest hardware boundary when designing robot integration.

Current handoff evidence establishes the multimedia/interaction foundation and ongoing stability work. It does
not establish completed chassis integration or coordinated robot behavior. Do not report overall completion
percentages until the full scope and acceptance criteria are defined; a working screen pet or voice conversation
alone does not complete this project.

## Source map

Host paths below are relative to `firmware/espressif/main/`. Detailed design guides are currently in Chinese.

| Area | Source | Documentation |
|---|---|---|
| Build tools | `tools/` (repository root) | [Build and flashing](docs/development/flashing.zh-CN.md) |
| Host architecture | `device/`, `runtime/`, `platform/` | [Architecture](docs/design/architecture.zh-CN.md), [source guide](firmware/espressif/main/README.zh-CN.md) |
| Guest apps and SDK | `guest/apps/`, `guest/sdk/`, `guest/runtime/` (repository root) | [Guest](guest/README.zh-CN.md), [SDK](guest/sdk/README.zh-CN.md) |
| Wire protocol | `runtime/abi/`, `runtime/services/` | [ABI](guest/abi/README.zh-CN.md), [header](guest/abi/micropixel_abi.h) |
| System UI | `host/ui/`, `host/controller/` | [Source guide](firmware/espressif/main/README.zh-CN.md) |
| Graphics | `platform/graphics/`, `platform/lvgl/`, `runtime/graphics/` | [Performance](docs/development/graphics-performance.zh-CN.md) |
| Host memory | `platform/memory/` | [Architecture](docs/design/architecture.zh-CN.md) |
| App storage | `runtime/bundle/`, `runtime/bundlefs/` | [BundleFS](docs/design/bundlefs.zh-CN.md) |
| Audio | `guest/apps/<app>/audio/sfx.json` (repository root) | [Audio specification](docs/development/game-audio.zh-CN.md) |
| Boards | `platform/boards/` | [Flashing](docs/development/flashing.zh-CN.md), [S3 support](docs/development/esp32-s3-box-3-bring-up.zh-CN.md) |

## Boundaries

- Guest SDK, apps, and ABI must not depend on ESP-IDF, LVGL, or board types, or introduce threads, mutexes,
 system calls, or direct hardware access.
- Before adding a helper to a Guest app, check the [SDK capability catalog](guest/sdk/README.md#capability-catalog).
 Apps must not re-implement catalogued capabilities (math, deterministic RNG, object pools, tone sequencing,
 virtual gamepad, coordinate mapping); a helper that a second app needs moves into `guest/sdk/` with tests.
- Keep SDK-to-wire conversion in Guest Runtime. Extend Service methods, channels, or events before adding
  Core imports. Never repurpose published IDs or expose C++ layouts, STL types, or Host pointers through the C ABI.
- The Host validates pointers, lengths, handles, generations, ownership, and capacity independently of SDK checks.
- Real-time and cross-task Host paths use fixed-capacity queues, arrays, or pools; no implicit growth or detached tasks.
  Scene submission and Raster upload may explicitly grow PSRAM storage after validation. Allocation failure preserves
  old resources; drawing does not allocate; app termination releases storage.
- Host APIs prefer `string_view` and `span`. When Host code must own growable text or collections, use
  `PsramString`, `PsramVector`, or `PsramMap` so storage lands in PSRAM. Process-lifetime fixed-size Host
  objects use `MICROPIXEL_EXT_RAM_BSS` (empty when the target has no PSRAM BSS). Real-time paths still use
  fixed capacity. `PsramBuffer` is the fallible, explicitly sized trivially-copyable buffer.
- Host task entry points and their callees must not put large arrays, structs, or temporary copies on the stack.
  Keep fixed workspaces in task-owned PSRAM contexts (or process-lifetime PSRAM BSS with explicit ownership).
  Audit aggregate initialization, assignment, return-by-value and by-value arguments for hidden stack copies.
  Do not bypass stack-frame checks or simply increase task stacks to accommodate work buffers.
- Use move-only RAII or an explicit shutdown protocol. Destructors perform best-effort cleanup. Do not use raw
  new/delete for real-time resource ownership. Exceptions and RTTI are disabled.
- ISRs record minimal POD state and wake tasks; they never call WAMR, Guest code, or LVGL.
  Board singletons and their input/control members stay in internal SRAM; move large task-only state to
  separate PSRAM storage, never mark the entire Board with `MICROPIXEL_EXT_RAM_BSS`. IRAM-safe handlers
  require the complete call chain and every dereferenced object (including queue storage) to be cache-safe.
  Avoid large synchronous logs on Guest hot paths.
- The Host owns App Hall, status overlays, system gestures, brightness, and device master volume.
  Guests must not add app-wide master attenuation. Define game sound parameters only in `audio/sfx.json`.

## Workflow and validation

1. Run `git status --short`. Preserve unrelated and uncommitted changes.
2. Check existing contracts and tests; make the smallest correct change. Update design and regression coverage
   before changing architectural boundaries. Follow the [C/C++ style guide](docs/development/code-style.zh-CN.md).
3. Run the checks below. Compilation alone does not establish protocol, lifecycle, or hardware correctness.
4. Report behavior changes, key files, commands and results, and outstanding hardware checks. Keep docs current.

| Change | Required checks |
|---|---|
| Documentation | Relative links and `git diff --check` |
| Guest SDK / ABI | Build Guests for the selected target and run relevant conformance tests |
| Firmware / System Shell | Host tests, formatting, and the selected board’s `build-host`; no Guest or App Store rebuild for Host-only changes |
| Shared graphics / LVGL / PPA branches | Validate the selected target; add other boards only for a concrete affected platform branch or an explicit multi-board request |
| Bundle / integrated app | Relevant tests and the app's release Bundle; build Host only if changed |
| Sound effects | Analyzer unit tests, release Bundle, hardware A/B listening |
| Graphics performance | Guest and Host stage measurements plus visible output; CPU usage alone is insufficient |

Run Host tests only through `bash tools/tests/test_firmware_host.sh`, never by invoking clang++ directly.
Formatting: `bash tools/check_firmware_style.sh --format-only`. Before release or push, run checks relevant to the change and release targets.
Use the board specified by the user or established by the current hardware task. Do not default to P4 or build
unrelated boards. For S31 work, use `bash tools/s31.sh build-host` and, when requested, `flash-host`.
Run relevant regressions once before release and record the result; rerun affected checks only when inputs change.
Do not duplicate them in board build jobs, SDK publication, or ordinary PR CI. Cloud SDK regressions are manual;
keep release artifact, installer lifecycle, source consistency, and checksum validation.
See [Contributing](CONTRIBUTING.md) for additional checks.

Activate ESP-IDF 6.1 via `export.sh` and configure WASI SDK and matching WAMRC before building.
A missing environment is not a Host build failure.
Host builds enforce stack-frame limits and emit GCC `.su` reports next to object files. For changes to
installation, signature verification, downloads or OTA, inspect the affected call chain and measure task
minimum free stack on the selected board after success and failure paths; a frame limit is not a total
call-stack bound. Record test results outside design docs; do not commit generated stack reports. After adding Kconfig symbols, inspect generated configuration.
Before hardware operations, read the flashing guide and identify the chip by MAC, not a re-enumerating port name.
Only one serial tool may own a device at a time.

## Repository hygiene

- Edit source JSON, assets, or generators. Do not edit or commit build outputs, artifacts, managed_components,
  generated sdkconfig, dependencies.lock.*, resource packs, AOT/Wasm/Bundle/Flash images, or generated reports.
- Update the pinned WAMR submodule only when requested. Do not copy its source into this repository or reformat
  third-party code. Check new dependency licenses and update [third-party notices](THIRD_PARTY_NOTICES.md).
- Never commit secrets, personal absolute paths, device identifiers, MAC addresses, raw serial logs, or one-off measurements.
- `guest/apps/mario/` is an optional, gitignored local benchmark; it must not become a required dependency.
- Default documentation filenames contain English; Simplified Chinese uses `.zh-CN.md`. Keep AGENTS.md in English.
  Write concise task guides and design contracts. Keep progress reports, conversations, and acceptance plans out of design docs.

## Active ESP32-P4 Bear task handoff

This section is a temporary continuation record for the current hardware task. Read it before investigating or
building so completed work is not repeated. Update or remove it when the task materially changes. Continue working
until the current five-hour usage window is nearly exhausted when the user requests persistent work, but never use
usage-reset credits.

### Delivery checkpoints

Treat these as review and acceptance boundaries, not as a requirement for one commit per directory:

1. The Function EV board/profile and Ubuntu build baseline are already present in branch history. Current board,
   partition, sdkconfig and build-file changes extend that baseline for Voice, display sleep and stability work;
   assign each hunk to the feature it enables instead of claiming the board port as new work.
2. Deliver Voice/System as one end-to-end contract across ABI, Device contracts, Host, Runtime, Guest SDK and the
   composition root. Voice service ID 24, the complete Voice method/event table and System 1.1 OpenSettings must not
   be split merely by directory. The Voice Gateway may remain a separately testable deployment unit.
3. Deliver Bear, the fixed Chinese font repertoire and settings behavior as one product checkpoint after the service
   contracts they consume are available.
4. Deliver the audio/AFE, Direct Surface lifecycle, Wi-Fi recovery, PSRAM admission and display-sleep stability work
   as a validated checkpoint. Split it further by subsystem only when every intermediate state builds and its focused
   tests pass; mixed-file hunks do not justify a broken intermediate commit.
5. Finish documentation and this handoff after code grouping and validation evidence are final.

### Resume here — checkpoint 2026-10-15

- The AFE starvation fix is complete and has passed hardware validation. The newest completed change is the
  persistent screen sleep selector: System Settings -> Power Management -> Screen sleep timeout, with 1/2/5/10-minute
  choices and a 2-minute default. The final Host image was built, flashed, and hash-verified. The device was last left
  at App Hall with the timeout restored to 2 minutes; Bear remains installed. Recheck connection and Wi-Fi state at
  the next session rather than assuming they remain unchanged.
- The blue screen followed by reboot associated with Bear lifecycle transitions was the main unresolved problem at
  this checkpoint. It was subsequently reproduced, addressed by the Direct Surface retiring/drain fix, and validated
  with 40 hardware lifecycle cycles on 2026-10-19; see the later follow-up below. Keep this earlier capture as root-cause
  evidence, not as the current task state. The user clarified the operation as returning to App Hall and tapping Close
  on Bear. One captured panic actually occurred during a subsequent Bear launch after a display idle suspend/resume
  cycle. Do not equate the user's observed trigger with the exact crash site.
- The captured exception was a core-0 Load access fault. Symbolization with the ELF from that capture resolved
  `block_size` in TLSF -> `multi_heap_get_info_impl` -> `heap_caps_get_largest_free_block` ->
  `micropixel_log_heap_state` (`runtime/wamr/diagnostics.c`) -> `ActiveHost::ActivateSelectedApp`
  (`host/controller/host_controller.cpp`). This suggests corrupted heap metadata or an invalid heap traversal;
  neither the corrupting writer nor the root cause is proved. The ELF has since been rebuilt for the settings
  feature, so old program-counter addresses must not be decoded against the current ELF as if they matched.
- Earlier memory snapshots did not show exhausted internal SRAM or PSRAM. A `direct_surface: control job timed out`
  message was also observed in the reproduction sequence. These were investigation clues, not proof by themselves
  of a specific race, use-after-free, DMA overwrite, or display-bandwidth fault. The first software candidate assigned
  synchronous presenter control jobs monotonic IDs, rejected stale completion signals, and propagated Direct Surface
  suspend failure through the graphics and Guest lifecycle layers. The later fix added the required sticky retiring /
  drained ownership state and passed the 2026-10-19 hardware validation described below.
- If a lifecycle failure reappears, inspect stop/suspend/relaunch ownership and synchronization for DirectSurface,
  Guest frame buffers, display detach/rebind, and outstanding rendering/DMA work. Preserve a matching ELF and complete
  panic dump locally before another rebuild. Do not generalize the 40 successful cycles into proof that every blue
  screen cause is fixed, and do not merely remove heap-statistics logging and claim a cause fixed.
- The earlier blue frame during voice playback and choppy reply audio remain separate unverified regressions:
  DPI is currently 48 MHz, and voice playback uses a 200 ms prebuffer / 1-second capacity. A connected spoken reply
  test still needs listening plus playback low-water/overflow counters and physical-panel observation.
- The user stopped continuous monitoring. No serial monitor was intentionally left running at handoff; do not
  assume old process/session IDs remain usable. The previous raw panic capture was held in conversation tool state,
  not a durable repository artifact. Check current processes before attaching any serial client.
- This final request is a documentation handoff only. Do not start hardware work merely from reading this checkpoint;
  use the next user's instructions to resume. Never use usage-reset credits.

### Voice-load investigation — 2026-10-15 follow-up

- A connected multi-turn capture recorded 17 playback summaries, 13 task watchdog reports and eight AFE empty-ring
  warnings. Playback low-water counts were nonzero in several turns; no playback overflow was reported. The user
  observed blue flashes and animation stalls while speaking. The raw capture contains no DPI underrun message.
- The same capture then recorded a PSRAM AXI read-address-invalid/misaligned abort immediately after stopping the
  suspended Bear from App Hall and unloading its AOT module. This is not proof of a bandwidth problem. The saved ELF
  hash matches the panic's ELF hash; watchdog PCs resolve into ESP-SR neural-network kernels and Opus SILK encoding.
- Local diagnostic artifacts are in `/tmp/bear-dialog-monitor/` (restricted permissions); do not commit raw logs.
  The original ELF/bin and pre-change voice source were preserved there. Monitoring automation `bear` and its owned serial collector were restarted after flashing the candidate.
  Do not attach a second serial owner. The pre-patch capture is also saved as `serial-baseline.log`.
- A candidate voice-load patch now checks the local-command preference inside `FeedLocalCommands`, reduces Opus
  encoder complexity to 3, inserts bounded blocked intervals in analyzed-frame and sender loops, and reports per-session
  analysis/encoding timing. It does not alter the 48 MHz display timing or fix the unproven PSRAM/lifecycle root cause.
  Host regressions, target build, architecture/format checks and diff whitespace checks passed; hardware
  voice/blue-flash validation is pending. Candidate flashing and hash verification passed, and Bear was started.
  Wi-Fi was not connected at the initial post-flash query. Candidate ELF/bin are preserved separately with
  `micropixel-candidate` filenames. Do not call the symptoms fixed.

### Bear AOT launch failure — 2026-10-16 follow-up

- A later Bear launch failed because the Guest PSRAM admission guard saw about 13 MiB free but a zero largest-block
  result and rejected WAMR's 41,088-byte module allocation before calling the allocator. The same launch attempt had
  already completed larger PSRAM allocations, so the zero value was inconsistent with actual allocator behavior.
- The candidate fix treats a zero largest-block result with nonzero free PSRAM as unavailable telemetry: it keeps the
  configured Host reserve, lets the real allocator validate contiguity, and applies the same fallback when deriving
  the Guest linear-memory ceiling. A one-shot warning records this state. Nonzero largest-block results still enforce
  the normal fragmentation limit.
- Host regressions and the ESP32-P4-Function-EV-Board build passed. The Host was flashed and verified; Bear then loaded
  its AOT module with the 41,088-byte allocation in PSRAM. Fifteen USB-driven start/stop cycles succeeded. The retained
  log window contained the final six AOT loads and seven cleanups with no WAMR allocation failure, AOT load failure,
  watchdog, panic, zero-largest snapshot, or fallback warning. This validates the launch guard fix but does not by
  itself prove the earlier lifecycle heap-corruption investigation complete.

### ESP-Hosted Wi-Fi boot recovery — 2026-10-19 follow-up

- Boot captures showed that saved-network discovery could see the access point while a cached targeted connection
  returned `WIFI_REASON_NO_AP_FOUND`; later attempts could associate without producing an IP event. Accepted connect
  commands previously had no deadline, so the settings UI could remain in Connecting indefinitely.
- The current candidate uses all-channel selection for user and automatic saved-network connections, arms a bounded
  connection deadline, retries with a full scan, and restarts the station after repeated no-event timeouts. Explicit
  cancellation now disconnects the in-flight attempt instead of only hiding the password dialog. Disconnect reasons
  in the no-AP family also force full-channel retry.
- Host regressions, architecture/format checks and the ESP32-P4-Function-EV-Board build passed. The candidate was
  flashed and write-verified. A reboot test recovered networking automatically after the first associations failed to
  produce an IP event; no panic, abort or watchdog occurred. The remaining ESP-Hosted/DHCP cause is not proved fixed,
  and automatic recovery latency should be tracked in later startup tests.

### Direct Surface lifecycle crash — 2026-10-19 follow-up

- The blue-screen/reboot was reproduced on the first USB-driven Bear start after an earlier lifecycle sequence. The
  Host restarted without a Guest `last-error`, consistent with the earlier Host-side PSRAM/heap fault.
- `DirectSurfacePresenter::Destroy()` previously returned success and cleared its release sink even when its barrier
  timed out. `DirectSurfaceService` then freed Host buffers and Guest teardown released WAMR memory while the presenter
  or DMA could still reference those pixels. Destroy now enters a sticky retiring state, rejects new frames, preserves
  ownership and callbacks after a timeout, and supports a later drain retry. Session teardown retries timed-out drains
  before freeing buffers. The service exposes destroy failures instead of unconditionally retiring the handle.
- Host regressions, firmware format/architecture checks and the ESP32-P4-Function-EV-Board build passed. The Host was
  flashed and verified. Hardware validation completed 20 direct Bear start/stop cycles, 10 immediate
  start/suspend-to-Hall/stop cycles and 10 one-second foreground start/suspend-to-Hall/stop cycles. The latter each
  scanned out about 19–20 frames. Uptime remained continuous, the Hall screenshot was valid, heap snapshots returned
  to the same baseline, and retained logs contained no control timeout, panic, abort, watchdog or heap-corruption
  report.

### Scope and safety

- The active board is ESP32-P4-Function-EV-Board. The current USB serial node is `/dev/ttyACM0`; verify it before use.
- Keep hardware tests quiet. Do not play speaker test tones unless the user is warned immediately before the test.
- Never print or commit API keys, Bearer tokens, `.env` contents, `api.txt`, device MAC addresses, or Voice Profile data.
- The worktree contains a large set of intentional, uncommitted changes. Do not reset, clean, checkout, or overwrite them.
- Before using the serial port, run `fuser -v /dev/ttyACM0`. A piped `micropixel logs` process previously remained
  alive after its parent command timed out and made the device appear disconnected. Terminate only a process that
  this task started and has positively identified.

### Completed implementation

- Bear has a local settings UI entered by holding the awake, idle ball for five seconds. Progress begins after about
  0.5 seconds; release, movement over 20 pixels, voice activity, or state change cancels it. A sleeping touch only
  wakes Bear. Voice-active touches retain cancel/interrupt behavior. Entering or leaving settings is silent.
- The settings UI includes status, behavior, sound, local commands, privacy, diagnostics, and about pages. It uses
  the existing HostSurface/Raster path rather than LVGL.
- A `zh-CN` UI font component is installed in NOR and has been visually verified without missing-glyph boxes.
- System Service 1.1 and Guest `System::OpenSettings` are implemented. On hardware, About -> System Settings stops
  the Guest cleanly and opens the Chinese Host settings screen.
- Bear voice-active touch handling now precedes settings routing, active Voice states leave settings, and animation
  intensity cycles through bounded values (`<50 -> 50`, `<75 -> 75`, `<100 -> 100`, otherwise `25`) instead of
  turning the default 65 into an invalid 115.
- System SDK calls that existed in 1.0 open System 1.0; only OpenSettings requires 1.1. Voice 1.0 calls similarly use
  Voice 1.0, while preferences, diagnostics, and context management require Voice 1.1.
- The System endpoint accepts the non-null base pointer produced by WAMR for a zero-capacity response and validates
  capacity rather than pointer identity.
- Non-P4 builds select `host/voice/voice_engine_stub.cpp`; ESP-SR headers and implementation are P4-only.
- Voice status output is redacted. It exposes configuration state, insecure-transport permission, revision,
  certificate fingerprint, and token suffix, but never the gateway URL or credentials. CLI regression coverage exists.
- The gateway requires an explicit `VOICE_DEVICE_TOKEN`; it has no default production token. Tests inject their own
  test-only token.
- The current Bear release Bundle was rebuilt and installed. It uses 1024x600, 96 vertices, AA, and two buffers.
  `/tmp/bear-after-update.png` is the last verified normal pet-screen capture.

### Checks already completed

Do not rerun these solely to rediscover the baseline. Rerun after affected inputs change or before final delivery.

- `bash tools/tests/test_firmware_host.sh`: complete pass after fixing strict aggregate-initializer failures in old tests.
- `python3 -m unittest tools.tests.test_font_tools`: 18 passed.
- `python3 -m unittest tools.tests.test_micropixel_cli tools.tests.test_cli_json`: 78 passed.
- `bash tools/build_guest_p4.sh`: Guest conformance and RISC-V AOT builds passed.
- Bear release AOT and Bundle build passed; the Bundle size was 196608 bytes.
- Voice Gateway suite passed 19 tests in its existing test environment. System Python lacks pytest; do not install
  packages globally merely because `/usr/bin/python3 -m pytest` fails.
- `bash tools/check_firmware_style.sh --format-only`: architecture and 537-file format checks passed.
- The final Function EV screen-timeout revision passed the complete Host test script, architecture/format checks,
  selected-board build, and `git diff --check`. Flash hash verification passed; about 7% of the app partition remains.
  This was a Host-only change; the installed Bear Bundle was preserved.

### Resolved ESP-SR AFE starvation

The original hardware failure repeatedly logged:

```text
AFE: Ringbuffer of AFE is empty, Please use feed() to write data
```

ESP-SR model initialization itself succeeds: `vadnet1_medium`, `wn10_hiesp`, and `mn7_cn` load; AFE reports one
16 kHz microphone and a 512-sample feed chunk. Both `voice_sr_feed` and `voice_sr_detect` tasks exist.

One real defect has already been fixed in
`platform/boards/esp32-p4-function-ev-board/i2s_audio.hpp`: the board wrapper did not forward the continuous capture
methods to its codec. `BeginCapture`, `ReadCapture`, and `EndCapture` now forward to `codec_`. This fix was built and
flashed, but at that stage AFE starvation remained. Do not repeat this patch and assume it is the complete cause.

Observed after that fix and before the final resolution:

- `voice_sr_feed` has very little runtime and appears blocked in capture startup or `esp_codec_dev_read`.
- `voice_sr_detect` keeps calling `fetch_with_delay`; during a Bear run the empty-ring warning reached roughly one
  line per 70 ms and could saturate the USB control path.
- The warning also occurs at the App Hall, though less frequently.
- After the last deliberate hard reset, the device was at App Hall with no active Guest. The updated Bear remains in
  the App Store.

The defect is resolved in the current worktree and flashed image. Do not repeat the investigation unless the behavior
regresses. The complete cause and fix were:

- The P4 board wrapper initially failed to forward continuous capture; that fix alone exposed two shared-I2S lifecycle
  defects. Capture-only RX needs TX to remain the clock master, but an empty TX DMA descriptor chain made later Bear
  playback time out. The Audio Engine then shut down the shared codec, leaving the speech feed blocked.
- Capture-only reads now enqueue matching muted TX frames so the DMA chain remains writable. Playback start and stop
  use an atomic output-transition waiter flag so the equal-priority feed task yields the codec mutex when either
  transition needs it. Playback stop keeps TX enabled while continuous capture owns it.
- Speech feed runs above detect, emits one bounded summary per eight seconds plus one-time milestones, and detect waits
  for an eight-chunk generation warmup at startup and after capture takeover. The board wrapper has compile-time
  assertions proving all three continuous-capture methods are overridden.
- Hardware validation passed at App Hall and with Bear in the foreground: requested and actual frames matched, begin,
  read and AFE feed failures remained zero, feed stabilized at 16 kHz, and there were no AFE empty-ring warnings,
  audio-output errors, watchdogs, panics or reboots during the final observation. `/tmp/bear-afe-fixed.png` is the final
  verified Bear screen capture.

A later end-to-end voice attempt exposed the missing stop-side waiter: `PauseOutputForCapture()` could time out in
`I2sCodecAudioSink::Stop()` while the equal-priority feed task repeatedly reacquired the codec mutex. Audio Engine then
closed the shared codec and AFE starvation returned. The output-transition waiter now covers both `Start()` and
`Stop()`. After rebuilding and flashing, an offline Bear session logged a successful output pause, six seconds of
exclusive capture, output restart, and resumed 16 kHz feed with zero failures and no empty-ring warnings. Later
online testing found a second stop-lock timeout during Wi-Fi reconnect; `Stop()` now waits up to 500 ms, and
`ReadCapture()` yields between muted TX DMA chunks when an output transition is pending. Detect also waits for eight
consecutive successful feed chunks after a feed error. Host tests, formatting and Function EV build passed; the
updated image was flashed. Bear then launched across Wi-Fi reconnect, and real conversations reached capture, upload,
response playback, and a detected `Hi ESP` wake word. A later session reported an ASR final-response timeout at the
gateway, so provider reliability is not established by this hardware pass.

On 2026-10-14, a user observed one brief blue-screen stall during reply playback. This resembles DSI FIFO underrun
under PSRAM bandwidth contention, but there is no direct underrun counter or captured blue frame to prove the cause.
The Function EV EK79007 DPI clock in `board_hardware.cpp` was reduced from 52 to 48 MHz. Architecture/format checks
and the Function EV build passed, and `flash-built` verified the new Host image on the board. The board remained up
for about 58 minutes with Bear rendering normally (roughly 22–24 FPS, no audio drops), a normal screenshot, and an
active 16 kHz AFE feed. Wi-Fi was disconnected during this observation; reply-playback/blue-frame regression remains
unverified and requires a connected voice session plus a human watching the physical panel. Do not treat a screenshot
of a normal frame or lack of display errors in the logs as proof that a transient blue frame is fixed.

The next connected voice session reached wake, capture, 112-frame upload, and reply playback. The user clarified that
the newly reported "recovery is very laggy" symptom was choppy reply audio, not a choppy display transition. The
playback path had only 60 ms start prebuffer and 200 ms total capacity at 16 kHz, with no underflow statistics. Its
prebuffer is now 200 ms and capacity 1 second; session-end logs report playback packet count, low-water reads,
overflow, and high-water milliseconds. Architecture/format checks and Function EV build passed, and the Host image
was flashed and hash-verified on 2026-10-14. The board rebooted and reconnected to Wi-Fi. A post-flash spoken reply
test is still pending; do not claim the audio stutter is fixed until the user hears it and `voice playback` counters
have been checked. The 48 MHz blue-frame regression is also still unverified by a physical panel observation.

On 2026-10-15, Function EV screen idle suspend gained a separate persistent setting under System Settings -> Power
Management. The choices are 1, 2, 5, and 10 minutes, defaulting to 2 minutes. The menu remains available on Function EV
even though whole-device idle sleep is disabled there; its page shows only the screen timeout control. Existing v1/v2/v3
Host settings records migrate to the 2-minute default. Host tests, format/architecture checks, and the Function EV build
passed. The image was flashed and hash-verified. On hardware, the default appeared as 2 minutes, selecting 5 minutes
survived a reboot and was applied by the display controller, then the setting was restored to 2 minutes. In a timed
idle run, the panel detached about 120 seconds after the 2-minute setting was applied and resumed in 212 ms. This screen
timeout work does not fix the separate blue-screen/panic issue. A captured relaunch panic after a Bear close cycle was a
core-0 Load access fault in TLSF heap traversal during `micropixel_log_heap_state` from `ActivateSelectedApp`; the writer
of the suspected heap corruption has not yet been identified. Do not describe that crash as a proven memory-exhaustion or
DPI-clock defect.

Screen-timeout implementation locations (relative to `firmware/espressif/main/`):

- `host/ui/system_settings_store.cpp`: 8-byte settings record upgraded to v4, retaining v1/v2/v3 loading.
- `host/ui/system_ui.hpp`, `host/ui/system_shell.*`, `host/controller/host_controller.cpp`: model, capability,
  action handling, startup application, and persistence.
- `host/ui/lvgl/square_common/{system_menu_ui,power_management_ui,square_system_ui}.*`: Function EV menu visibility,
  selection UI, and board callback; localized strings are in `host/ui/i18n/`.
- `platform/boards/esp32-p4-function-ev-board/{platform,display_idle_controller}.*`: runtime timeout setter and
  atomic timer value. Source Kconfig/defaults now use 120 seconds. The existing generated build config retained its
  older 30-second boot fallback; Host startup overrides it with the persisted value or 2-minute default, verified
  in hardware logs. Do not manually edit generated sdkconfig.

Local screenshots, if still present: `/tmp/function-ev-screen-sleep-dropdown.jpg` shows all four choices,
`/tmp/function-ev-screen-sleep-persisted.jpg` shows 5 minutes after reboot, and
`/tmp/function-ev-screen-sleep-restored.jpg` shows the final 2-minute selection. These establish UI behavior,
not absence of transient blue frames.

Do not begin a spoken ASR or speaker test until the user has been told that the test is about to start.

### Build environment and commands

ESP-IDF:

```bash
export PATH=/home/flo/projrct/embedded/esp32p4/.toolchains/node-v22.23.3-linux-x64/bin:$PATH
export IDF_TOOLS_PATH=/home/flo/projrct/embedded/esp32p4/.toolchains/espressif
export IDF_PYTHON_ENV_PATH=/home/flo/projrct/embedded/esp32p4/.toolchains/espressif/python_env/idf6.1_py3.14_env
source /home/flo/projrct/embedded/esp32p4/.toolchains/esp-idf/export.sh >/dev/null
python3 tools/firmware.py esp32-p4-function-ev build
```

WASI/AOT:

```bash
export WASI_SDK_PATH=/home/flo/projrct/embedded/esp32p4/.toolchains/wasi-sdk-33.0-x86_64-linux
export WAMRC=/home/flo/projrct/embedded/esp32p4/.toolchains/wamrc-riscv32-ilp32f/wamrc
```

Flash the already-built Host only after a successful build:

```bash
python3 tools/firmware.py esp32-p4-function-ev flash-built --port /dev/ttyACM0
```

Build and install Bear:

```bash
python3 tools/micropixel package guest/apps/bear --profile release --aot-target riscv32-ilp32f \
  --output-dir build/apps/bear --output build/apps/bear/bear.bundle.bin --force
python3 tools/micropixel --port /dev/ttyACM0 app install build/apps/bear/bear.bundle.bin \
  --start --wait-timeout 120
```

### Remaining work after the AFE defect

- Start with the remaining connected voice regression: listen to spoken replies while recording playback
  low-water/overflow counters and observing the physical panel for blue flashes or animation stalls. The Direct
  Surface lifecycle fix already passed 40 hardware cycles; investigate it again only if lifecycle symptoms recur.
  Do not rerun completed baseline investigations or builds. After new changes, run only the affected checks:
  Host-only work needs Host tests, formatting, and the Function EV build; Guest/ABI changes also need the relevant
  Guest conformance and Bear Bundle checks.
- Hardware-regress Chinese UI, five-second settings entry, animation-intensity persistence, About -> Host Settings,
  voice-active cancellation, and absence of watchdogs/reboots.
- Finish currently shallow settings features: diagnostic recording and saved downlink-audio tasks, live microphone
  diagnostics, license details, and verify that touch-sound and streaming-response preferences affect real behavior.
- Preserve the established sound rule: the first touch that wakes sleeping Bear may cue; touching the screen or Bear
  while already awake must not replay the wake cue.
- Voice Profile NVS storage is still plaintext and is a later security-hardening item. Do not combine that redesign
  with the AFE fix.
- Complete Voice 1.1, System 1.1, font-component, and settings-entry documentation after behavior is stable.

### 2026-11-05 maintenance batch

This batch began as software-only and was followed, at the user's request, by one `flash-built` operation.
No serial monitor was left running, no speaker test was performed, and the Voice Gateway was not accessed.

- Fixed conformance/ABI drift: `dynamic_texture` now expects `VERSION_MISMATCH` for both an excessive minor
  and a higher Resource major, then opens the current Resource 1.0. Stale Graphics/Audio version labels were
  removed from test and SDK comments.
- Bear: `kTouchSounds` now plays quiet listen/curious/excited cues only on a real mood change; waking from
  sleep still plays only the `kWakeCue` cue, and awake touches never replay it. `kStreamingResponse` is now a
  read-only Host-managed row instead of an ineffective toggle. Bear README matches the implementation.
- Docs: Network 19 is documented as an implemented 1.0 service with its Profile/AppId/time prerequisites;
  the docs index and SDK capability catalog include the missing Network, Voice, and development guides; the
  architecture statement about Host-owned surfaces checking the Guest reserve was corrected.
- Direct Surface screenshots: the presenter now fills its own panel-size staging instead of a caller stack
  object, so a timed-out capture job cannot touch memory the caller has freed.
- Voice: WebSocket and session-thread reads of `session_id_`/`generation_` now use one mutex-protected
  snapshot; cancel message formatting uses caller-local storage. Remaining transport teardown/StopPcm/AFE
  join gaps are documented, not fixed.
- `AppSessionError::kLaunchBitmap` and its two mappings were removed as dead code; the device-layer launch
  bitmap hooks remain.
- Security fix: `RasterService::UploadWarp` validated sizes in 32 bits, so a 32768x32769 map wrapped to a
  ~262 KiB allocation followed by a multi-GiB `fill_n` write. Sizes and the Guest length check now use 64
  bits and reject maps that cannot fit the uint32 allocation interface; a regression test covers it.

Checks run for this batch: `bash tools/tests/test_firmware_host.sh` (pass), the ASan/UBSan
`raster_service_test` (pass), `direct_surface_service_test` and `pcm_stream_service_test` (pass),
`bash tools/check_firmware_style.sh --format-only` (architecture and 537-file format pass), the Guest P4
conformance build (25 guests, pass, output in `/tmp`), the Bear release build (pass, output in `/tmp`), and
the ESP32-P4 Function EV Host build (pass). The rebuilt ELF
`3ec91542255081d8842bbede2dd7ff8f11475b5f82227c77a64a68c5c60cf52d` (`micropixel.bin`
`088e4021fab49e5ba2e5484ac9e88953ef5acf548fbe6d79b08519afd62e49d0`) was then flashed with
`flash-built`; esptool verified the written data and the board hard-reset and re-enumerated. No boot,
Wi-Fi, or Bear observation was made after flashing, so hardware validation of these changes remains open.
The older 2026-10-19 ELF `703f4f9684e24f7d0b30d4b1498ee05f31dd7b6a1b2772d5bf4aa4a0947e26e7` is
preserved under `~/.codex/private/micropixel-risk-20261105/` for old logs only; do not symbolize old
panics with the new ELF. Gateway reachability, AFE empty-ring triage, and the display-sleep lifecycle
sequence also remain open.

### Bear launch/stop capture — 2026-11-12 follow-up

- A no-reset long serial monitor captured a Core 0 Load access fault during a Bear launch. The device reports ELF
  prefix `3ec915422`, matching `build/host-esp32p4-function-ev/micropixel.elf` at SHA-256
  `3ec91542255081d8842bbede2dd7ff8f11475b5f82227c77a64a68c5c60cf52d`. The default
  `build/host-esp32p4/micropixel.elf` is stale for this board and emitted a flashed/built checksum mismatch; discard
  its initial `gdma_new_link_list` symbolization.
- Correct symbolization maps MEPC `0x402d7220` to TLSF `block_size` and RA `0x402d7238` to `tlsf_walk_pool`, matching
  the earlier heap-metadata traversal failure. Logs place it after `launching selected App: micropixel.bear`; the
  corrupting write is still unknown.
- The captured external Host stop path requested cooperative Guest stop, waited 3 seconds, then force-terminated the
  Guest. Direct Surface destruction, WAMR cleanup and return to Hall completed in the observed stop sequence. This
  does not establish whether a stop before the captured launch fault was the cause.
- One Bear launch after boot was observed without a fault. No source code was changed, built or flashed in this
  follow-up. The monitor was closed and the serial port released. The full raw panic output was not persisted; next
  capture should tee one long monitor session to a restricted temporary log and explicitly use the matching
  Function EV ELF.

### Heap-check diagnostic follow-up — 2026-11-12

- A light heap-check build passed Hall rendering, then found invalid TLSF metadata while walking the heap at
  `before Hall launch cover retention`, before Bear Guest execution. A WAMR full-init before/after checkpoint was
  added to `wamr_runtime.cpp` for diagnostic builds only; this workspace change is diagnostic and uncommitted.
- The comprehensive heap-check build scanned/verified heaps around Guest service calls. IDLE1 remained in
  `multi_heap_check -> verify_fill_pattern`, triggering Task WDT and repeated blue-screen/recovery cycles. Treat
  these watchdog resets as instrumentation-induced, not as proof of the original corruption path.
- Stopped the comprehensive diagnostic monitor and restored the normal Function EV `flash-built` image. esptool
  verified writes and hard-reset the board. No post-flash boot/Bear regression was observed; original TLSF/PSRAM
  failure remains open. The serial port was released with no monitor left running.
- Continue with narrow, low-overhead checkpoints. Do not run full heap verification on every Guest ABI call; keep
  the matching ELF and one long serial connection for the next capture. Full results are in
  [`handoff/README.md`](handoff/README.md#41-2026-11-12-堆检查诊断固件的后续结果与回退).
