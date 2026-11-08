# QR login renewal and recovery

Checked on 2026-11-08 against installed TikTok LIVE Studio 1.37.4. The plugin
uses this desktop passport QR flow for both its LIVE Studio and Android output
profiles; importing cookies does not replace the saved Android device identity.

## Official desktop behavior

Static inspection of `static/js/main.af7b32f7.js`, webpack module `197750`, shows:

- New/scanned codes are checked again after 2 seconds.
- `expired`, `refuse` and `error` statuses request a new code (`FailRetry`).
- A thrown check failure without `error_code` is retried while a counter is below
  5, then requests a new code (`CheckFail`). A failure with `error_code` requests
  a new code immediately. Failure to obtain a code exposes manual refresh.
- There is no fixed local expiry timer in this component: renewal follows the
  returned status. This does not establish the server's token lifetime.

The account adapter in `static/js/24040.83dfd9ae.js` queries its regional domains,
prefers a reply matching `client_secret`, and otherwise prioritizes confirmed,
scanned, expired/refuse/error, then new. This explains why stale `new` replies
must not hide expiry. Confirmed cookies must still belong to this login secret.

The original Python application's current source behaves differently:
`F:/Apps/TiktokStreaming/TiktokStreamKeyGenerator.py:7600` stops on expiry and
asks for manual refresh. The plugin now follows the official desktop renewal
behavior rather than preserving that limitation.

## Android APK comparison

The original APK workspace is `/home/loukious/Android/TikTok` in Ubuntu WSL.
`analysis/apk-manifest.json` identifies `com.zhiliaoapp.musically` 47.1.4,
build 2024701040. `apks/split_df_a_dex.apk` SHA-256 is
`383070f3d5299d1f2efd50be2ee678d686251ecb000f91b0c330b76a6abbbb7d`;
its `classes19.dex` SHA-256 is
`c28f0c485804d0072db916e471f2ad937fd797ead1fdf840f74f56b6df90781f`.

The APK's native tablet QR display differs from LIVE Studio: expiry stops
polling and displays a refresh control. A tap requests another code; reopening
an expired/error view also regenerates. Waiting checks are delayed 1100 ms,
or 2000 ms after scanning. The phone-side scan response reads `expire_time`;
these inspected classes do not establish a fixed server lifetime.

Existing JADX 1.5.6 recovered the targeted classes into
`/tmp/obs-qr-apk-evidence` in Ubuntu. Evidence anchors: `1ECK.java:29` (expiry),
`1ECL.java:44` (delays), `QRCodeLoginViewModelV2.java:59` (generation),
`QRCodeLoginViewModelV2.java:96` and `1ECS.java:9` (reopening),
`AwS564S0100000_33_I1.java:4978` (expired overlay),
`ACListenerS174S0100000_33_I1.java:2442` (refresh click), and
`QRCodeLoginVerifyFragment.java:277` (`expire_time`). These are ordinary JADX
static findings, not REA Evidence records or observed app execution. Relevant
methods were recovered; shared dispatcher classes had unrelated method errors.

## Plugin policy and verification

The plugin automatically replaces expired/refused/error codes. Temporary signer
or TikTok transport/HTTP 408/5xx errors retain the displayed code for six delayed
retries (5, 10, 20, 30, 30, 30 seconds), then try one replacement. These longer
delays are a plugin policy because every check consumes hosted signing requests.
Signer 401/403/429, invalid signatures, certificate errors, and failed replacement
generation stop and show the error. Cancellation prevents further requests.

`native-tiktok-test` exercises these states, matching-secret regional selection,
and confirmation after an outage. `qr-login-test` runs the actual Qt dialog and
worker against offline HTTP fixtures, checks scheduled timer intervals without
waiting for them, and verifies renewal, cookie saving, stopping and cancellation.
No live account or RapidAPI request is used by these tests. Existing connection,
media-signing, LIVE safety and dual-output suites also passed.

New cookie exports use `cookies_<username>.json` and a browser-compatible JSON
array. Filenames are sanitized, collisions receive a unique suffix, and a
temporary file is renamed without overwriting an existing export. The native
fixtures verify filename handling, JSON attributes, round-trip import and
preserving existing files. The dialog fixture verifies account-name lookup when
confirmation omits it, and preserves successful login if that optional lookup
fails.

## Retained desktop evidence

The source was inspected as inert text; the app was not executed. Source SHA-256:
`AE9D43C7D530C75F22262F2E0053559ED0453B979BEF09CFBAF1559C0585BEC5`.
The QR module occupies UTF-16 offset 3136799, length 3243 in the installed main
bundle. An unchanged module body with an object-literal wrapper and provenance
record is retained in `build_x64/qr-studio-source/`.

REA CLI 6.5.0 analyzed that extraction; complete local output is
`build_x64/qr-studio-ui-evidence.json`, Evidence ID
`ev_9f0976e3b95d208f7e77c71d70c1a45809ce35ec1cf620dce0b62b3dace46032`.
This is static evidence with unresolved external/obfuscated dependencies, not a
runtime observation or a measurement of TikTok's server expiry duration.
