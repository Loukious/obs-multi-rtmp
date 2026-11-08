# TikTok dual layout

Implemented in native C++/Qt for OBS 32.2. One target owns two outputs, one TikTok room, one Start/Stop control, one heartbeat, and one signature cache. Each output has an independent video encoder and signed SEI session.

## User controls

- Single stream remains the default. Dual is offered for desktop LIVE Studio and Android Camera LIVE. Streamlabs and Android screen capture do not support this mode.
- The Service, Video Settings, Audio Settings, Output and Other Settings tabs remain present in both layout modes. Portrait and Landscape appear side by side in Video Settings and in independent Audio Settings.
- Portrait and Landscape each select a scene by UUID, even when audio is shared. Names include their canvas. Removed scenes remain unavailable and are never silently replaced.
- Each layout has dimensions, Fit/Crop, a native preview, H.264/HEVC encoder, bitrate, frame-rate divisor and the full OBS encoder property editor. A private canvas renders the selected scene through a wrapper composition. Selecting a scene does not switch OBS's main program scene.
- Use the same audio for both layouts starts enabled. The common track (1–6), AAC encoder and bitrate are selectable.
- With sharing off, both layouts have independent track, encoder and bitrate controls. Equal track choices are permitted. First use copies the common settings; subsequent toggles preserve each independent selection.
- Audio comes from OBS's active program mixes. The private video canvases do not add duplicate audio to those mixes. Users route sources in Advanced Audio Properties. Choosing a scene does not automatically isolate its audio.
- Both feeds upload audio even when they share an encoder; the UI shows their combined configured upload bitrate.

## Protocol and ownership

| Layout | TikTok room endpoint |
| --- | --- |
| Portrait | `stream_url.rtmp_push_url` |
| Landscape | `multi_stream_url.rtmp_push_url` |

Both endpoints must be valid and distinct. This applies to creation, resumed rooms and recovery after a lost create response. The original app's `apply_stream_outputs` in `F:/Apps/TiktokStreaming/TiktokStreamKeyGenerator.py` establishes the endpoint roles.

Signed eligibility checks use `/webcast/game/basic/create_info/` with `scene=1` and `/webcast/anchor_tool/threshold/check/`. Eligibility requires `allow_multi_stream && (allowed || dual_canvas_used)`, with explicit successful responses. Missing or malformed results cannot unlock dual. The account panel shows Can Dual Layout for every native TikTok account, even before dual is selected. Unknown/locked access disables the dual option; credential changes and Refresh immediately invalidate old results. A new-user lock explains the reported remaining 25-minute sessions and up-to-48-hour validation delay, matching the original app. Startup independently checks access again.

Desktop and Android Camera LIVE send `multi_stream_scene=1`, `multi_stream_source=1` and `live_studio=1`. Android retains its mobile request profile and version 2.0 frame metadata. Actual encoder dimensions go into each output's metadata. One fetched signature batch feeds two distinct metadata sessions, preserving independent indices and timestamps.

A newly created or recovered room belongs to this attempt. A compatible pre-existing room is adopted only after both outputs start. Failed inspection/startup of an unadopted pre-existing room does not end it. Account/session fingerprints and room reservations prevent local targets from using the same authenticated session or room simultaneously. Fingerprints are memory-only hashes; credentials are never logged by this mechanism.

Startup prepares both canvases, encoders and outputs before creating a room. It starts both feeds only after endpoint validation and signature prefetch. Successful `obs_output_start` is treated as a request, not proof of live delivery: the controller waits for both start signals. A 60-second deadline bounds output startup. Reconnection status is tracked per feed. A terminal failure, missing scene or explicit Stop stops the pair and cleans up the room once. Network preparation is cancellable; uncertain create responses are recovered before cleanup. Each attempt owns its signal callback data until disconnection, so late callbacks cannot address a replacement attempt.

## Persistence

Each target has an optional `dual-layout` object containing `enabled`, `same-audio`, `independent-audio-initialized`, `shared-audio`, `portrait` and `landscape`. Layouts embed scene UUID/name, dimensions, framing, encoder, bitrate, `fps-divisor`, full encoder `param` objects and independent audio settings. These embedded settings replace the originally proposed global configuration references and ensure independent encoder ownership. Existing single-mode `video-config` and `audio-config` references remain unchanged. Old configurations load as single-stream with audio sharing enabled. The editor restores a saved dual selection after access is reverified.

## Validation

### Primary OBS integration

`tiktok_primary_service` selects `tiktok_primary_output` through OBS's preferred-output API. A Qt adapter checks the native widget/slot contract before adding the provider and reusable TikTok controls to the Stream page. It delegates native Apply validation first, commits only when OBS clears its changed state, and rolls back newly prepared canvases if native validation rejects the save. Cancel performs no preparation or commit. The frontend service getter is borrowed; the adapter acquires its own reference. Primary credentials and layout choices are persisted in the streaming service and profile, separately from dock targets.

Native canvases are registered with the frontend using `ACTIVATE | SCENE_REF`, without `MIX_AUDIO`. Creation nests the selected scene with Fit/Crop framing. Existing canvases must own the selected scene and match output dimensions; streaming never deletes or resizes them. Preview uses a temporary composition without changing the persistent canvas's selected scene.

The primary output waits for both transport start signals before announcing OBS streaming. It shares the landscape feed's encoders for OBS capture accounting, with no third video encoder. Single mode shares OBS's selected video/audio encoders with one transport. Reconnect notifications and total bytes/dropped frames reach OBS's normal status UI. Both TikTok feeds retain distinct packet metadata and share room ownership, signing cache, heartbeat and cleanup. A primary session reserves the same account lease as dock targets. Stream delay is rejected at service initialization before OBS begins delayed capture. Main-window closure waits for preparation cancellation and transport cleanup before frontend teardown.

The runtime suite also covers registered primary single/dual outputs, delayed-start rejection, encoder counts, persistent native canvas reuse and creation rollback, arbitrary audio tracks, reconnect notifications, pending start cancellation, partial failures, shutdown cleanup, and Apply/Cancel/provider switching. `primary-frontend-smoke` is an opt-in test module for an isolated portable OBS installation; it checks the actual Settings widget contract, ordinary service Apply, invalid TikTok credentials, managed-service reload and switching away. It requires `TIKTOK_PRIMARY_SMOKE_REPORT` and starts no broadcast. Never install this test module in a user's OBS installation.

`connection-settings-test` covers visibility, scene identity, preview dispatch, arbitrary/equal tracks, sharing toggles and serialization. `native-tiktok-test` covers eligibility, endpoint mapping, desktop/mobile dual requests, existing-room mismatch and lost-response recovery. `media-signing-test` retains the original signed packet fixtures.

The Windows `dual-output-test` initializes real libobs D3D11 canvases, synthetic colored scenes, two distinguishable audio sources on OBS tracks 5 and 3, local capture codecs and an in-memory transport. HTTP fixtures intercept all requests. It checks actual Fit/Crop pixels, separate scene rendering, selected and swapped audio amplitudes, equal tracks with distinct encoders, shared encoder ownership, per-feed metadata dimensions and index zero, one signature batch/heartbeat, duplicate account rejection, reconnect status, immediate and delayed output failures, startup timeout, cancellation, scene deletion, resource release and one room finish sequence. The primary OBS stream stays stopped.

The test also opens the complete editor, checks all five tabs in both modes, opens/saves real encoder properties, tests account denial and verifies that saved advanced properties and frame-rate divisors reach the actual encoders. The optional third argument to `dual-output-test` saves full editor and native preview screenshots when run with `QT_QPA_PLATFORM=windows`. The UI suite can also save both shared and independent settings screenshots.

Tests do not publish to TikTok. Production ingest, both playback layouts and hardware encoder limits must still be verified on the intended account during a live test. The native reference is `Smile-4-Me/tiktok-live-obs` at `3d7e7e9ec77b0260b2dde4af726d3b15c5a4d933`.
