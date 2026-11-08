## OBS-Multi-RTMP
This fork of [OBS-Multi-RTMP](https://github.com/sorayuki/obs-multi-rtmp) adds TikTok Streamlabs and native TikTok LIVE Studio / Android connections.

## TikTok features

- **Primary or additional streams:** use TikTok through OBS's normal streaming controls or add independent destinations in the Multiple output dock.
- **Three TikTok connection options:** Streamlabs, LIVE Studio, and Android Camera LIVE or mobile gaming, with background account checks and automatic device setup.
- **QR login:** scan and confirm on your phone. Expired codes renew automatically, and temporary connection failures retry while keeping the code visible.
- **Named cookie exports:** successful QR logins save `cookies_<username>.json`. Repeat logins add a suffix to preserve existing files, and exported JSON works with both LIVE Studio and Mobile.
- **Portrait and landscape together:** eligible LIVE Studio and Android Camera LIVE accounts can stream dual layouts with independent scenes, resolutions, encoders, and optional separate audio tracks.
- **LIVE controls:** topic selection, searchable games, gifts, comments, and content disclosure settings for native TikTok connections.
- **Account and session feedback:** Streamlabs eligibility, native dual-layout eligibility, RapidAPI quota, and detailed provider errors.
- **LIVE violations and appeals:** automatic notifications while streaming, manual violation checks, and account-verified appeal submission where supported.

## How to use it
For TikTok as your main OBS stream, open **Settings → Stream**, set **Streaming provider** to **TikTok**, and choose Streamlabs or LIVE Studio / Mobile. Enter the method's credentials and stream details, then **Apply**. Use OBS's normal **Start Streaming**, **Stop Streaming**, and streaming hotkeys. Single mode uses OBS's Video and Output settings. Switching the provider back to **Default** restores the ordinary service form.

For primary dual layout, use LIVE Studio or Android Camera LIVE with an eligible account. Choose **Dual layout — Portrait + Landscape**. In the Video tab, select a scene for each layout and either **Create from selected scene** or an existing OBS canvas. Apply creates persistent native portrait and landscape canvases that remain after streaming stops and are saved with the scene collection. Existing canvases retain their resolution. In the Audio tab, sharing defaults on; turn it off to choose any OBS track independently for each layout. Native OBS status receives aggregate transport statistics and reconnect notifications. Disable OBS stream delay for this managed service. Primary TikTok settings are stored with the profile, separately from dock targets.

After restarting OBS, Apply, layout preview and stream startup restore video for saved TikTok canvases using the saved dimensions. The scene selectors show the original input scenes; generated TikTok compositions stay internal. Changing a source, resolution or framing rebuilds only that layout, while unchanged layouts retain their saved canvases.

To add TikTok as a secondary destination instead:

1. Install the plugin as usual.
2. Open OBS and go to `Docks` -> `Multiple output`.
3. Click on `Add new target`.
4. In the Service tab, choose **Connect using**:
   - **Custom URL / stream key** (default): enter your server and stream key. Protocol and authentication controls belong to this method.
   - **TikTok — Streamlabs**: enter a required Streamlabs token from [StreamLabsTikTokStreamKeyGenerator](https://github.com/Loukious/StreamLabsTikTokStreamKeyGenerator), then set your title and category.
   - **TikTok — LIVE Studio / Mobile**: choose the desktop or Android client, enter a required RapidAPI key, and browse to a TikTok cookie file. Use the [TikTok LIVE Studio API signer](https://tiktok-live-studio-api-signer1.p.rapidapi.com/) subscription associated with your key.
5. Click **OK**, then **Start** in the Multiple output dock. TikTok methods obtain the server/key automatically.

Both TikTok methods show the account username below their credentials. Account checks run in the background after inputs change; **Refresh** retries the check or rereads an updated cookie file. Streamlabs uses `GET /api/v5/slobs/tiktok/info` (`user.username`). LIVE Studio and Mobile use the signed `GET /passport/account/info/v2/` endpoint with the selected cookie session. Android automatically registers missing IDs before its account check. Account checks never start a LIVE. A lookup failure does not prevent saving the connection. Custom URL/key connections retain their service's authentication username field.

Streamlabs also displays **Status** and **Can go live** from `application_status.status` and `can_be_live`. Missing information displays **Unknown**. Its category field is a searchable dropdown with an **Other** option. As in the original generator, requests send only the first 25 Unicode characters of the search; full category names remain selectable. The selected `game_mask_id` is saved and used directly when starting the stream.

LIVE Studio / Mobile follows the **Topic** and **Game** controls in the original TiktokStreaming app. Topic starts blank and is required: Gaming, Music, Chat & Interview, Beauty & Fashion, Dance, Fitness & Sports, Food, News & Event, or Education. Gaming shows a required, searchable Game dropdown for LIVE Studio and Mobile screen capture. Games load asynchronously from TikTok's `/webcast/room/hashtag/list/`; search matches any part of the name, ignoring case, and selections restore by API ID. Unmatched names cannot be used as games. **Refresh** retries a failed list request. Camera LIVE hides Game and omits game/screen-capture fields from its requests. Other topics send game ID `0` where applicable.

LIVE Studio / Mobile also offers **Allow gifts**, **Allow comments**, and **Content disclosure**: Off, Own brand, Third-party brand, or Both. Gifts and comments default on; disclosure defaults off. These choices persist in both the main OBS settings and Multiple output targets, and apply to new rooms in single or dual layout. Resuming keeps the existing room's settings. Requests use the original app's client-specific disclosure fields. If TikTok explicitly reports that disabled gifts or comments stayed enabled, startup fails and cleans up the new room before sending stream data. Streamlabs does not expose these native room controls.

Cookie files may use Netscape format or browser-export JSON (an array, or an object with a `cookies` array). TiktokStreaming exports with blank or missing cookie domains are also supported and scoped to TikTok during import. Export cookies after logging into TikTok. Cookies explicitly belonging to other domains are ignored; refreshed cookies remain in memory for the current session. Tokens are masked in the form and saved in the plugin's local configuration.

In either the main OBS settings or a Multiple output target, **Log in with QR code** signs in through TikTok's LIVE Studio passport flow. Enter your RapidAPI key, scan the code with TikTok on your phone, and confirm. A successful login saves a new cookie file under the plugin's configuration directory and selects it automatically for LIVE Studio or Mobile; existing cookie files and Android device identities are preserved. Codes renew automatically when TikTok reports expiry, refusal or a QR error. Temporary signer or TikTok polling failures keep the code visible and retry up to six times with increasing delays of 5–30 seconds before requesting a replacement code. Key, subscription and quota rejections, or failure to generate a replacement, stop polling and show the error. Login can be cancelled at any time.

New QR logins save browser-compatible JSON as `cookies_<username>.json`. A suffix preserves any existing file for that username. If TikTok does not supply the username, a background account lookup fills it; if that lookup is unavailable, the confirmed login still saves using an account ID or unique filename.

For Streamlabs, **Detect Streamlabs account** imports the latest token saved locally by Streamlabs Desktop, following the original generator's newest-log and last-token selection. The masked token field then triggers the normal username, status and Can go live lookup. If detection finds nothing, the existing token remains available for manual editing. Choosing a cookie with Browse or importing login details marks the main OBS settings as changed so Apply can save them.

Streamlabs, LIVE Studio and Mobile show the detailed provider error when startup fails, including nested TikTok explanations such as an already-active LIVE room. Native account, eligibility and signing errors also retain the service's explanation and available status codes.

LIVE Studio and Mobile follow the original TiktokStreaming app's hostname rules: the login's `store-idc` takes priority, Singapore aliases normalize to `c-alisg`, and TikTok domain discovery supplies fallback routes with separate desktop and Android parameters. The discovered `priority_region` is independent of the account/store region and hostname. Region context stays with the login and client when device IDs are generated; a prepared LIVE keeps its successful Webcast host for heartbeats and cleanup.

Leave both ID fields blank for automatic setup. LIVE Studio registers when starting; Android generates a local hardware/installation profile and registers directly with TikTok before its account check or stream startup. Android registration uses native gzip/ttEncrypt, without a signing API request. The returned IDs, fingerprint and registration metadata appear in the editor and are saved with **OK**; startup also saves the profile, including after a failed attempt so retries reuse the local identity. **Refresh** retries failed Android setup. Manual IDs remain optional, and each client's profile is saved separately. Mobile offers **Camera LIVE** (default) and **Mobile gaming (screen capture)**, with **Portrait** or **Landscape** orientation for screen capture. Mobile uses a TikTok 47.1.4 request profile and requires the signer's Android frame-signature version 2.0 support.

The streaming settings window resizes its contents with the window and keeps the size you choose when changing tabs or receiving account results. Audio dropdowns expand to use available space. The **OK** button stays outside the scrolling settings.

LIVE Studio / Mobile requires OBS 32.2 and an H.264 or HEVC encoder. Requests, heartbeats, frame signatures, and video SEI insertion run in native C++ using Qt and libcurl. There is no Python runtime or external FFmpeg process. Existing Streamlabs checkbox settings migrate automatically, and switching methods preserves each method's inputs.

Native components are adapted from [Smile-4-Me/tiktok-live-obs](https://github.com/Smile-4-Me/tiktok-live-obs), revision `3d7e7e9`. Attribution and its GPL-3.0-only license are in [data/licenses](data/licenses/tiktok-live-obs.txt). The repository's original license file is retained.

LIVE Studio / Mobile shows a **RapidAPI quota** bar below the API key in both editors. It displays the remaining allowance, turns red at 10% or less, and shows the reported reset date and last update in its tooltip. Request and frame-signing responses update the meter without extra API calls, including responses that report exhausted quota. The last observation survives restarts and is kept separately for each API key and signer; no key is stored in the quota cache. Until headers arrive it shows **Unknown**. Usage from other apps and resets are reflected by the next response, rather than estimated locally.

LIVE Studio / Mobile also includes **Show a popup for new LIVE violations while streaming**, enabled by default in both editors. It checks active violations every 15 seconds while OBS is focused, or every 60 seconds in the background or after a request error. Checks use RapidAPI signing quota. One final check runs after stopping or disconnecting, and repeated records do not reopen the popup during that session. **Check violations…** opens the same window manually.

The popup leaves OBS's controls usable and shows the violation, appeal status and available actions. **Submit appeal** uses the original app's general or snapshot appeal requests, with account and eligibility checks before submission. General appeals offer the same reasons and 140 UTF-16-unit limit for **Other**. Copyright restrictions link to TikTok's official evidence form. Submissions are explicit and single-attempt; an unconfirmed response displays the error and requires a status refresh. This feature requires the cookie login and RapidAPI key used by LIVE Studio / Mobile; a Streamlabs token alone does not provide those credentials.

## TikTok dual layout

In **LIVE Studio / Mobile**, **Can Dual Layout** checks account access automatically, including in single mode. It uses the original app's two gates: `allow_multi_stream` from `game/basic/create_info/?scene=1` and `allowed || dual_canvas_used` from `anchor_tool/threshold/check/`. Dual mode stays disabled while access is unknown or locked. **Refresh** checks again; a new-user restriction shows the remaining 25-minute LIVE sessions and possible 48-hour validation delay.

Choose **Dual layout — Portrait + Landscape**, then open **Video Settings**. Portrait and Landscape appear side by side, with independent scenes, resolutions, H.264/HEVC encoders, bitrates and frame-rate divisors. Both layouts initially select the current OBS scene, and may use the same scene. **Encoder settings…** opens the encoder's full property editor, including preset and keyframe controls where supported. Scenes on additional canvases include their canvas name; selections are saved by UUID. **Fit** preserves the whole scene with empty space where needed; **Crop to fill** fills the output without stretching. **Preview layout** shows the selected composition. These choices do not switch OBS's main program scene.

Choose a scene for each layout before saving dual mode. Apply/OK opens the relevant Video or Audio tab and identifies the layout and field if a scene, encoder, or other layout setting is missing or invalid. Cookie errors stay in the Service tab.

In **Audio Settings**, **Use the same audio for both layouts** starts enabled. Choose the common OBS track, AAC encoder and bitrate. Uncheck it to choose any track **1–6**, encoder and bitrate independently in side-by-side columns. Both modes also expose the audio encoder's full settings. Equal track selections are allowed. Independent settings survive switching sharing back on. Tracks carry OBS's active program audio, routed through **Advanced Audio Properties**; selecting different video scenes does not isolate their audio or add another copy to the program mix. The Video, Audio, Output and Other tabs remain accessible in both layout modes; Other contains sync-start and sync-stop.

One target controls both streams. **Start** checks account eligibility, prepares the room and starts both feeds with independent video signing state, one signature cache and one heartbeat. The dock reports each feed's status and bitrate. If either feed fails permanently, times out during startup, or loses its selected scene, both stop. **Stop** ends both feeds and the owned room. Startup can be cancelled. The primary OBS stream does not need to be running.

Dual layout requires TikTok account access and two distinct ingest endpoints. Supported methods are desktop LIVE Studio and Android **Camera LIVE**. Streamlabs and Mobile screen capture remain single-stream. Existing single-stream settings are preserved. Dual settings are stored inside each target's `dual-layout` configuration, including separate saved audio settings for shared and independent modes.

With `ENABLE_CONNECTION_TESTS=ON`, the Windows `dual-output-test` runs real libobs canvas rendering, audio mixing and packet callbacks against local capture codecs and offline TikTok responses. It checks swapped/equal/shared tracks, Fit/Crop, signing dimensions and counters, duplicate sessions, cancellation, partial startup failures and room cleanup. Actual TikTok delivery and hardware encoder capacity still require a live test on the intended account.

## Support the original developer
### [paypal / 贝宝](https://paypal.me/sorayuki0)

### alipay / 支付宝
![alipay](./docs/zhi.png) 

### wechat / 微信
![wechat](./docs/wechat.jpg)

## Support the TikTok extension developer

TikTok Extension By: **Loukious** ([donate](https://buymeacoffee.com/loukious/)).

[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://buymeacoffee.com/loukious/)

## Build

This project uses obs-plugintemplate.   
Please refer to obs-plugintemplate to understand how it works.
