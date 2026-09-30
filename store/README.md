# App Store listing material (DOSDeck)

Everything App Store Connect needs for the iOS app, which is **DOSDeck** —
not Retro-DOS. Android's listing lives in `play-store/`.

| File | Field | Limit |
|---|---|---|
| `description-en-GB.txt` | Description | 4000 |
| `subtitle-en-GB.txt` | Subtitle | 30 |
| `keywords-en-GB.txt` | Keywords | 100, commas included |
| `../ios/REVIEW-NOTES.md` | Review Notes | — |
| `screenshots/<target>/` | Screenshots | exact sizes only |

## Review notes live in ios/, and there is only one copy

There used to be a second set here, `store/review-notes.txt`, and it was
deleted rather than updated. It opened "The app you last reviewed was built
with Flutter" — written for a reviewer of the **Retro-DOS** submissions. On
DOSDeck that is false (the record is new and has no review history) and worse
than useless: it argues the two are the same app, which is exactly what
guideline 4.3 rejected the family for. `ios/REVIEW-NOTES.md` is the only copy.

## Screenshots

Captured by the `screenshots` job in `.github/workflows/ios.yml`, never by
hand. Raw simulator captures land in `screenshots-raw-iphone/` and
`screenshots-raw-ipad/`; `tool/make-store-screenshots.py` pads them to the
exact sizes App Store Connect accepts and writes `screenshots/<target>/`.

Landscape only, because the app is: `Info.plist` declares the two landscape
orientations and nothing else. An iPad capture must never be padded into an
iPhone frame — the script enforces that rather than trusting the operator.

`simctl` photographs the *device* framebuffer and the simulator boots
portrait, so a landscape-only app comes out lying on its side. The script
rotates it upright before sizing; that is why the raw files are portrait and
the finished ones are not.

Uploading — the display type must match the pixel size exactly, and
`asc screenshots sizes --all` is the list that settles it:

| Folder | `--device-type` | Size |
|---|---|---|
| `screenshots/ios-iphone-69-landscape` | `APP_IPHONE_69` | 2868×1320 |
| `screenshots/ios-ipad-13-landscape` | `APP_IPAD_PRO_3GEN_129` | 2752×2064 |

Both are produced native and unscaled, so nothing is padded.
