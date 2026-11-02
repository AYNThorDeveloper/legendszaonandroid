# Video settings on the AYN Thor

**Reference device: AYN Thor.** The menu is project-authored HTML displayed by the Android app's WebView, with native hooks for reading and saving settings.

![Project-authored video-settings menu preview](images/video-settings.png)

## What the screenshot shows

This is a browser capture of the exact menu HTML included in this repository, using illustrative values supplied by a small mock of the Android bridge. It is not a capture from a running game or a performance measurement. No game image, texture, character, official logo or firmware data is used. The small preview caption is added only by the documentation capture script.

Regenerate on macOS with Chrome installed using `python tools/capture_settings_preview.py`. Regeneration can change font/rendering bytes, so visually review the result and explicitly update the approved image digest in tools/publication_guard.py before publication. No screenshot is captured from a personal browser session; a temporary isolated Chrome profile is used.

## Open and navigate

Press L3 + R3 together on the AYN Thor. The implementation recognizes fresh presses within 300 ms. The private environment override `TOTK_VIDEO_MENU=0` disables the shortcut. Start/+ keeps its game function.

Use the D-pad to choose a row and left/right to adjust it. A changes the selected choice, Y applies the draft, and B returns. The current UI is in French: Résolution de rendu = render resolution; Résolution de sortie = output resolution; Limite d’images par seconde = frame-rate limit; Compteur FPS = FPS counter.

## Rendering versus output

Render resolution controls the game image's internal drawing resolution. Output resolution controls the final image size sent to the display. They can differ: a 1920 × 1080 output can present an image rendered at 1280 × 720. Increasing output alone does not establish native 1080p rendering. Resolution changes require restarting the game after saving.

## Frame-rate limits

The menu offers 30, 40, 60 and 120 FPS plus Auto. This selects a cap, not a performance guarantee. Scene complexity, shader compilation, driver behavior and the reference device's workload determine the achieved rate. Applying an FPS choice changes pacing immediately; resolution changes wait until restart.

## FPS counter

The FPS counter can be shown or hidden from the menu. This takes effect immediately after applying. Choosing 60 FPS in the menu does not prove that every scene sustains 60 FPS. The reported 50–60 fps at 1080p on the AYN Thor is documented separately in README.md.
