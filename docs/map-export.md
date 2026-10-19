# Map for sharing and My maps

Added 2026-10-18 on the owner's request: "users should be able to save bathymetric maps and the mosaic
to share them". The owner chose the first of two models: **sharing by file** -- the user saves the map
and decides who gets it and on what terms. A KOGGER community map (uploads merged, cleaned and published
by KOGGER) is the second model and is not built: it needs a user agreement first (see "Licences and
the law" below).

**The map is for KoggerApp first** (the owner, the same day: "as Garmin has it, only so that people can
exchange depth maps and mosaics"; Google Earth and GIS come second). So:

1. A user saves their survey as a **KOGGER map, `.kmap`** (Settings -> Export -> Map for sharing).
2. They send the file (a messenger, mail, a stick).
3. The other user opens it in KoggerApp (Open menu, drag and drop, the command line, or My maps ->
   Open a map; on a phone or tablet by tapping it in the messenger and choosing KoggerApp). It goes into
   **My maps** and lies **under their boat** in the 3D view: depth colours and isobaths, the mosaic if
   they want it; the depth under the boat by the map is a data field.
4. KMZ (Google Earth) and GeoTIFF (GIS) stay as "for other programs", off by default.

Claims are marked **[Verified]** (run or read), **[Inference]** or **[Assumption]**.

## Using it: saving

Settings -> Export -> **Map for sharing (.kmap)** opens the page (`qml/app/MapExportSettingsTab.qml`).
The folder is the Export group's (as for CSV). The page has:

| Setting | What it does | Kept |
| --- | --- | --- |
| Title | the map's name; also the files' name, with the export's date and time | yes |
| Author, Contact | written into every file; empty = anonymous | yes |
| Licence | All rights reserved (default) / CC BY 4.0 / CC0 1.0 | yes |
| **In the file:** Isobaths | the depth surface as the view draws it -- the isobath tool's edge limit, extra width and step (on by default) | yes |
| Transducer depth, m | added to every depth; > 0 makes the depths count from the waterline | yes |
| **In the file:** Side-scan mosaic, Mosaic detail | the mosaic (on by default) at 2, 4, 8, 16 (default) or 32 cm a pixel -- the view's zoom levels 2..6 | yes |
| KOGGER map (.kmap) | the file to share, for KoggerApp; on by default | yes |
| For other programs: Google Earth (KMZ), GIS (GeoTIFF) | off by default | yes |
| Without the track and times | for KMZ and GeoTIFF only, on by default: no track, the survey's date as a month. The .kmap never has the track and always gives the month (owner, 2026-10-18) | yes |

Settings live in QSettings group `mapExport` (`src/map_export/map_exporter.cpp`).

"In the file" (owner, 2026-10-18: the export marks what goes into the file, the isobaths and the mosaic):
two blocks under that heading, the isobaths first; with neither on, "Save the map" is greyed and the page
says there is nothing to save (`mapexport_page_check` test 6b) -- the exporter refused that case already
with a message.

On Android, once a save has written a .kmap, **Share the map** opens the system share sheet with it (a
messenger, mail): see "Android: from a messenger and back".

## Using it: opening (My maps)

- **Open**: the main Open dialog or Settings -> Files -> Open file ("Logs and maps", or "KOGGER maps";
  the Files one listed logs only until 2026-10-18, and a map opened there stays out of the recent logs),
  dropping a .kmap on the window, the command line (`KoggerApp.exe map.kmap`), or My maps -> Open a map. Every path ends in
  `MapLibrary::importFile`; `Core::openLogFile` hands a .kmap over before it closes anything
  (`Core::openMapFileInstead`), so opening a map never drops the survey or the device link. On Android
  also from another app: tapping the file in a messenger or sharing it to KoggerApp (next section); the
  in-app pickers there give `content://` URLs, often without the name, so `openMapFileInstead` tells a
  map by its first bytes (`startsLikeMap`).
- **Kept**: the file is copied into `AppDataLocation/maps` (`%APPDATA%\KOGGER\<app>\maps` on Windows) as
  `<title>_<sha1-10>.kmap`, then read from the copy: the same file twice is one map; a messenger's
  `content://` file is read once front to back, in 1 MB pieces (`mapexport::copyMapFile`), and survives
  its cache. What the file is comes from its content, never its name: one that does not start as a ZIP
  is refused before anything is written ("this is not a KOGGER map"), a ZIP without a KOGGER `map.json`
  after the copy (`readMapPackage`). A map just opened is flown to
  (`GraphicsScene3dView::fitGeoBox`; with no survey open the view's frame moves to the map, as a GeoJSON
  file's load does).
- **Shown**: the bottom toolbar of the 3D view has a fifth layer button, **My maps** (map icon): the eye
  shows or hides all maps, the gear opens the **My maps** page (`qml/app/MyMapsTab.qml`): show the maps /
  depths and isobaths / side-scan mosaic; a card per map (preview, title, author or "Author not given",
  licence, size, depth range, its own switch, "Show on the map", "Remove" with a second press to confirm);
  "Under the boat by the map: 7.4 m (title)". Layer switches: QSettings `mapLayer/*`; hidden maps:
  `mapLibrary/hidden`.
- **Depth by map**: a data field for the widgets (`DataFieldCatalog`, key `mapDepth`), from
  `MapLibrary::boatDepth`: bilinear in the newest visible map with a depth at `Dataset::getLastRawPosition()`
  (double), NaN off the map or over a gap.

## Android: from a messenger and back

Added 2026-10-18: the users are mostly on phones and tablets, so a map arrives in a messenger.
**Status: on the owner's phone (2026-10-18) receiving works from Telegram and WhatsApp, a map is kept
across a restart, and "Share the map" sends the file whole -- WhatsApp keeps it as `.bin`, which the
owner set aside ("not WhatsApp for now")** -- see the end of this section.

**Receiving** (tap the file in Telegram or WhatsApp, choose KoggerApp; or share it to KoggerApp):

- `platform/android/AndroidManifest.xml`: `VIEW` and `SEND` filters (on `KoggerOpenActivity`, below) for `application/vnd.kogger.map+zip`
  (the app's own label, below), `application/octet-stream`, `application/zip`,
  `application/x-zip-compressed`. Not by name: a messenger's `content://` URL often has none.
- Why octet-stream is the one that matters: Telegram for Android sends a document whose extension Android
  does not know as `application/octet-stream` (`SendMessagesHelper.prepareSendingDocumentInternal`), and
  opens a received one with `ACTION_VIEW`, the message's type, `FLAG_GRANT_READ_URI_PERMISSION` and
  `startActivityForResult`; with no type, or when no app takes the type, it uses `text/plain`
  (`AndroidUtilities.openForView`) **[Verified: Telegram's source, DrKLO/Telegram master, read
  2026-10-18]**. `text/plain` is not taken -- every text file would offer KoggerApp. WhatsApp's client is
  closed; that it behaves alike is an **[Assumption]**.
- The filters are on **`KoggerOpenActivity`**, a tiny activity without Qt (`Theme.NoDisplay`, no
  history, not in Recents) that hands the intent on to `KoggerActivity` with `startActivity` and
  `FLAG_ACTIVITY_NEW_TASK | CLEAR_TOP | SINGLE_TOP | GRANT_READ_URI_PERMISSION` and closes. Why, seen on
  the owner's phone (Xiaomi 2201116TG, Android 13, MIUI) on 2026-10-18 with the filters on
  `KoggerActivity` itself: Telegram -> the system chooser -> KoggerApp ME made a **second
  `KoggerActivity` in Telegram's task** (`wm_create_activity` in task 19096, the app's own was 19095)
  although it was `singleTask`; Qt 6.8 answers a second activity while it runs by restarting the app
  (`QtActivityBase.onCreate` -> `restartApplication()`: `makeRestartActivityTask`, `Runtime.exit(0)` --
  read in `Qt6Android.jar`), and the restart is a plain launch: the map was lost **[Verified: the
  device's event log and logcat, the jar's bytecode]**. That a launch asking for a result keeps even a
  `singleTask` activity in the caller's task is how AOSP's `ActivityStarter` reads (the result is kept,
  so no existing task is reused) **[Inference]**. Started from `KoggerOpenActivity` no result is asked
  for, so the running app is found in its own task (`onNewIntent`), or the app starts with the file.
  `KoggerActivity` stays `singleTask` (was `singleTop`): the launcher, USB and the hand-over then never
  make a second one.
- The read grant goes on with the hand-over (`FLAG_GRANT_READ_URI_PERMISSION` on the forwarded intent),
  so it is held by `KoggerActivity` and outlives `KoggerOpenActivity`: the map was read after
  `KoggerOpenActivity` had closed, both at a cold start and in the running app **[Verified on the phone]**.
- `KoggerActivity.handleOpenIntent` (from `onCreate` and `onNewIntent`): `VIEW` -> `getData()`, `SEND` ->
  `EXTRA_STREAM`, to the native `koggerOpenFile`. Not again for a restored activity or a task relaunched
  from Recents (`FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY`): a map removed from My maps must not come back.
- `AndroidInterface::jniOpenFile` (registered with the other natives in `setNativeMethods`) keeps the URL
  until `Core::notifyUiSettingsApplied` installs the handler (`setOpenFileHandler`): a map that started the
  app waits for the UI. Then `Core::openReceivedFile` on the GUI thread -> `MapLibrary::importFile`. Taken:
  the My maps page opens (`receivedMapOpened`, `MainWindow.qml`) and the view flies to the map. Refused: a
  warning with the reason ("Not opened: this is not a KOGGER map").
- Reading `content://` is Qt's: `QFile` opens such a URL through Android's content resolver **[Inference,
  Qt's documentation]**; the permission is the one the messenger granted with the intent, which holds
  while the activity lives **[Inference]**. The copy is made at once, so later reads are of the app's own
  file.
- My maps on Android says how: "tap it in the messenger and choose KoggerApp" instead of dropping on the
  window.

**Sending** (Settings -> Export -> Map for sharing -> Save the map -> **Share the map**):

- `Core::shareFile(path, "Send the map")` -> `AndroidInterface::shareFile` -> `KoggerActivity.shareFile`:
  `ACTION_SEND` with `mapexport::kMapMimeType` (`application/vnd.kogger.map+zip`), the file as
  `EXTRA_STREAM` and `ClipData`, `FLAG_GRANT_READ_URI_PERMISSION`, in the system chooser. The URL is from
  the declared FileProvider `${applicationId}.qtprovider`. The button shows only on Android and only when
  the last save wrote a .kmap; it shares the .kmap, not a KMZ or GeoTIFF beside it.
- Folders the provider covers (`res/xml/qtprovider_paths.xml`): the app's files, its external files and
  caches, and the whole primary shared storage (`external-path "/"`). The export folder is the Export
  group's, by default `DocumentsLocation/KoggerApp/exports`, which on Android is the app's own external
  folder, `/storage/emulated/0/Android/data/<package>/files/Documents/KoggerApp/exports` (covered by
  `external-files-path`) **[Verified on the phone: the export wrote there]** -- not the shared Documents,
  as first assumed. A file manager on Android 11+ does not show that folder and uninstalling the app
  deletes it, so on a phone "Share the map" is the way to get a map out. A
  folder none of them covers (a removable card: FileProvider throws `IllegalArgumentException`) is copied
  into `getCacheDir()/share` first (a new `cache-path` entry), replacing what the last share left there.
- The type: Telegram ignores the one given and sends octet-stream (above) **[Verified, its source]**;
  a messenger that keeps it would open the file on the other phone with KoggerApp alone, no chooser
  **[Assumption]**.

**On the owner's phone** (Xiaomi 2201116TG, Android 13, MIUI; `KoggerApp-1.0.3-me-20261018-172848.apk`,
package `org.kogger.koggerapp.me` beside the official KoggerApp and the old `.myedition`; with the owner,
over adb and logcat, 2026-10-18) **[Verified]**:

- Telegram, the app not running: the .kmap sent from Telegram Desktop arrived as `application/zip`
  (`content://org.telegram.messenger.provider/.../Telegram Documents/2_5377...kmap`), the system chooser
  -> `KoggerOpenActivity` -> `KoggerActivity` started in its own task with the file -> after the UI was up
  `map library: Opened: Map`. One `KoggerActivity` (`dumpsys activity`), no restart.
- WhatsApp, the app running: `application/zip` again, and the URL has no name at all
  (`content://com.whatsapp.provider.media/item/<uuid>`) -- told by content, as intended -> `onNewIntent`
  in the same process -> `map library: Opened: Map`; still one `KoggerActivity`, no restart.
- Before `KoggerOpenActivity` (APK of 16:53, filters on `KoggerActivity`): Telegram's
  `application/octet-stream` open restarted the app and lost the map (above).
- So both messengers sent this .kmap as `application/zip`, not `application/octet-stream`: Telegram
  Desktop types a file by its content **[Inference]**; a .kmap sent from Telegram for Android goes as
  octet-stream (its source, above) -- both are taken.
- Saving on the phone: the recording 00014 (pushed with adb, SHA-256 checked) -> Save the map: 11.1 s
  (mosaic 102 tiles, 79 with data, in 9.6 s; 4 270 soundings, 4 790 triangles, grid 418 x 339), written
  to the app's own folder (Sending, above), 2 972 551 bytes.
- **Share the map**: the system chooser got `SEND`, `application/vnd.kogger.map+zip`, the URL in
  `ClipData`, the read grant; it offered Telegram, WhatsApp, Yandex Mail, Messenger and Nearby Share --
  none turned the type away. WhatsApp took it, and its copy in `WhatsApp Documents/Sent` is the saved map
  byte for byte (SHA-256 `12d331e0...`), **but named `DOC-20261018-WA0005.bin`**: the name and `.kmap`
  were dropped. A file WhatsApp got from its desktop app kept its name (`Map_20261018-155147.kmap` in the
  same folder). Why: WhatsApp names a shared file it cannot type from the type, and our own type is
  unknown to Android's `MimeTypeMap` **[Inference]**. What the other side's chat shows was not seen. Such
  a file still opens in KoggerApp on a phone (by content); on a computer `.bin` does not open with
  KoggerApp. The owner set WhatsApp aside the same day; whether another type (`application/zip`,
  `application/octet-stream`) keeps the name was not tried.
- **Kept across a restart**: the map opened from Telegram (`Opened: Map`), `am force-stop`, the app
  started from the launcher: `map library: 1 maps` **[Verified on the phone, 19:33]**. So the earlier
  `0 maps` (18:21, after the same map had been opened at 17:38 and again at 17:52 as "Opened", not
  "Already in My maps") came from cards removed by hand **[Inference]**: `MapLibrary::remove` logged
  nothing then; it now logs `map library: removed <id>` (in the phone build of 19:13, not yet seen
  firing).

Not verified on a device: a non-map refused (the content test is verified on the desktop), what a
recipient sees of a shared map, `SEND` into KoggerApp from another app's share sheet, Android versions and
makers other than this phone. Also verified: the copy, the content test and both pages (Checks below); on the desktop
with a fresh profile, the .kmap from the command line copied byte for byte (SHA-1), a log named .kmap
refused with nothing kept, the same map again "Already in My maps"; every APK built passes
`apk_deps_check` on both ABIs and is signed with the test key; the manifest as installed (`aapt2 dump
xmltree`, `cmd package query-activities`) **[Verified, 2026-10-18]**.

The first Android build stopped in `map_layer.cpp` (My maps' drawing): `GL_R8`, `GL_RED`, `GL_RGBA8` are
OpenGL ES 3 names the GLES2 header lacks. It now includes `<GLES3/gl3.h>` on Android and `LINUX_ES`, as
`scene3d_renderer.h` does; the head unit's build would have stopped the same way **[Inference]**.

## The KOGGER map file (.kmap)

`src/map_export/map_package.h`. A ZIP archive, every entry **stored** (the app reads it with its own
`ZipReader`; Qt has no public raw-inflate):

| Entry | What |
| --- | --- |
| `map.json` | the passport: the fields of `metadata.json` below with `"format": "KOGGER map"`, `formatVersion` 1 (a newer one is refused with a message), `products.bathymetry.paletteTheme` the maker's depth colours (their preview's; My maps draws in the isobath tool's) |
| `depth.tif` | the GIS export's depth GeoTIFF (float32, -9999 none); with the times shown in the GIS files, the .kmap gets its own copy with the month only |
| `mosaic.tif` | the GIS export's mosaic GeoTIFF (8-bit index = the raw echo intensity, 0 none; the maker's palette for GIS -- My maps colours it with the opener's mosaic tool) |
| `preview.png` | up to 512 px: the depth colours, or the mosaic when there are no depths |
| `README.txt` | the notice and the licence in plain words |

**Nothing of the track** (owner, 2026-10-18: a shared map holds the isobaths and / or the mosaic, not
the boat's track nor the bottom profile under it). The .kmap never had the track or the 3D bottom track
-- the track only ever went into the KMZ -- and now its passport has no word of one either
(`survey.trackIncluded` / `trackAndTimeHidden` are left out, the lineage says "triangulation of the
soundings") and its survey dates are a month whatever the privacy switch says (`kmapMetadata` in
`map_export_writer.cpp`). `mapexport_check` holds it in both modes: exactly these five parts, no
"track" in map.json, no day in any part; two mutants (the full passport; the flags kept) are caught
**[Verified]**. An export of 00014 with the track allowed: the .kmap as above, the KMZ with its Track
folder **[Verified, 2026-10-18]**. On screen, the .kmap alone with the boat-track and bottom-track
layers on shows the isobaths only; the track and the bottom track seen over a map come from a recording
open at the same time **[Verified]**.

What remains of the survey in the file is in the data itself **[Verified, 00014]**: `depth.tif` holds
the soundings along the passes as they were measured and the linear triangulation between them, so a
hillshade of it shows the survey lines as creases; the coverage outline follows the outer passes and the
way in; the mosaic shows the passes by its nadir gaps. Hiding the lines in the depths would mean
smoothing them (the depths under the passes would no longer be the measured ones) -- the owner chose
to keep the measured depths (2026-10-18).

Shared on Android as `application/vnd.kogger.map+zip` (`mapexport::kMapMimeType`: the app's own label,
not registered). Every .kmap starts `PK\3\4`, the first entry's header -- that is how a nameless file
from another app is first told (`startsLikeMap`, `copyMapFile`); a log (KLF, XTF, UBX) does not start
so **[Inference: their formats' first bytes]**.

Read by `readMapPackage` with `readGeoTiff`, which reads what `writeGeoTiff` writes and nothing else
(tiled, deflate or none, 8-bit palette or float32, EPSG:4326). Renamed to .zip, the GeoTIFFs open in QGIS
**[Inference]**. On 00014 at 16 cm: 3.0 MB **[Verified]**.

## How the maps are drawn

`src/map_export/map_layer.{h,cpp}`: `MapLayer` (GUI thread) keeps each visible map in lat/lon -- the depth
grid thinned to at most ~400 x 400 vertices, the mosaic in 1024 px pieces of 8 x 8 quads -- and makes an
immutable frame projected into the view's frame (`Camera::viewLlaRef_`, spherical in perspective, Mercator
top-down, as `geojsonToScene`) whenever that frame or the maps change; `MapLayerGl` (renderer) draws the
latest frame and uploads its textures once per key. Wiring: `GraphicsScene3dView::setMapLayer`, the frame
handed over in `synchronize()`, drawn in `drawObjects()` right after the basemap tiles with the depth test
on and **outside** the `!isOut` block, so the maps stay in every camera state.

- Depths: a mesh at z = -depth with the surface view's own `isobaths` program (bands and lines by
  `fwidth`); normals carry the slope x30 because that shader draws lines only where the normal leans.
  **The isobath tool sets them** (owner, 2026-10-18: loaded isobaths follow the app's isobath tool):
  the step and the colour theme of Settings -> Isobaths and the toolbar's theme strip
  (`IsobathsViewControlMenuController::styleChanged` -> `MapLayer::setIsobathStyle`, wired in
  `Core::UILoad`); the maker's `paletteTheme` in map.json is no longer used for drawing. One depth scale
  for all the visible maps, so maps side by side agree: the palette (`IsobathUtils::colorPalette`
  expanded as `SurfaceProcessor::generateExpandedPalette`) runs over their joint depth range, and the
  bands start at a whole multiple of the step, so the lines lie at 2, 4, 6 m and not at odd depths as the
  live surface's do (it starts at its own shallowest point). More than 2048 bands (a 0.1 m step over
  more than ~200 m) doubles the step until they fit: the palette is a texture that wide.
- Depth labels on the isobaths, as on the live surface: the code moved out of SurfaceView into
  `src/scene3d/utils/isobath_labels.{h,cpp}`, shared by both -- candidates where a line crosses a
  triangle, levels of detail with spacing 2 x step doubling, on screen only the level the zoom asks for
  and without overlaps. For a map they are made once per style in the map's own metre frame (~200 ms at
  0.1 m on the 00014 map, 111 000 labels; 10 ms at 1 m) and only moved into the view's frame per frame.
  Not drawn while the maps' mosaic is shown, as the live surface does.
- Mosaic: the `mosaic` program, R8 index textures with mipmaps as SurfaceView's tiles, at z = 0 (the
  water surface): with both on, the mosaic covers the depths. **The mosaic tool sets its colours**
  (owner, 2026-10-18: the saved mosaic must be adjustable as the isobaths are, not a flat picture): a
  .kmap keeps the raw echo intensity per pixel, 0 = none, exactly the index the view's tiles hold, so
  the layer looks it up in the live mosaic's own table -- `mosaic::PlotColorTable` from the mosaic
  tool's theme and levels (`MosaicViewControlMenuController::styleChanged` ->
  `MapLayer::setMosaicStyle`, wired in `Core::UILoad`), one table for every loaded mosaic. The maker's
  palette in `mosaic.tif` stays for the preview and for GIS. **[Verified, 00014 .kmap alone, 2026-10-18]**:
  theme 1 with levels 10..90, then theme 4 with 40..60 -- `my maps mosaic: theme 4, levels 40..60` and
  the screen changes accordingly (the narrow window leaves most of the mosaic black, as it does live).
- Where the view's frame is the dataset's (near the survey, perspective), the map and the boat use the same
  formula, so they agree **[Verified: the depth map of 00014 under its own bottom track and start loop]**.

**Verified on 00014 (2026-10-18)**: a fresh profile (`receiver`) opened the .kmap from the command line --
"map library: Opened", flown to, the depths drawn with no survey open; then the recording: the map under
the boat, the bottom track on it; the mosaic layer alone: the file's mosaic under the boat matching what the
app paints itself.

**Verified on 00014 (2026-10-18), the isobath tool's style**: a test profile with theme 1 and a 1 m step,
then theme 8 and 2 m -- the log says `my maps isobaths: theme 8, step 2 m (tool 2), depths 0..22 m, 12
bands` and the screen shows the map under the boat in that theme with lines and labels (8.0, 10.0,
12.0) every 2 m; the receiver profile's theme 5 and 0.1 m came through the same way. The step reached
the layer through `styleChanged` at start (the settings page sets it after `UILoad`); a theme change
while running takes the same signal **[Inference: not clicked on screen]**. The move of the label code
out of SurfaceView was checked line by line (221 lines the same after the renames, the rest the same
statements as functions instead of lambdas); the live surface's labels were not seen on screen after
it: the test profiles built no live surface from 00014.

**The isobaths go as the app draws them** (owner, 2026-10-18: "as we drew the isobaths in the app, so we
send them"; the export's own "Join soundings up to" and "Isobaths every" are gone). The export takes the
isobath tool's edge limit, extra width and step as `DataProcessor` last passed them on
(`MapExportConfig::surfaceEdgeLimit`, `surfaceExtraWidth`, `surfaceStep`); the step colours the preview
and the KMZ, a .kmap's isobaths follow the opener's tool. Until 2026-10-18 the export had its own (50 m,
Auto) because the owner's view, 20 m and 0.1 m, leaves a survey with lines ~30 m apart in strips: that is
now what the file holds too, as on the screen -- to share a filled map, widen the isobath tool's edge
limit past the line spacing. **[Verified, 00014, mapexport profile with the owner's 20 m / 0.1 m / 5 m]**:
`map export: depths built (4273 soundings, 2138 triangles, 360187 extrapolated, grid 424 x 345; the
isobath tool's edge limit 20 m, extrapolation 5 m, step 0.1 m)`, the preview strips along the passes
with patches at the crossings, map.json `edgeLimit_m` 20, `extrapolation_m` 5. Not compared with the
screen pixel for pixel: the test profiles built no live surface from 00014 (see above).

## What is written (the KMZ and GeoTIFFs, "for other programs")

Files: `<Title>_<yyyyMMdd-HHmmss>.kmap` (above), and when asked `.kmz`, `_mosaic.tif`, `_depth.tif`.

**KMZ** (Google Earth on a computer or a phone): `doc.kml` first (a viewer takes the archive's first .kml),
`README.txt`, `metadata.json`, `files/legend.png`, the pictures.

- Side-scan mosaic: 1024 px pieces as indexed PNGs (the view's palette, index 0 transparent), each in a
  `Region` with `minLodPixels 128` when there are more than one, over a coarse overview of the whole
  (longest side 2048 px).
- Depth colours: the surface view's bands and theme (`IsobathUtils::colorPalette`, deepest band first, as
  `isobaths_colored.fsh`), hidden by default when there is a mosaic (they would cover it).
- Isobaths: one placemark per depth, `MultiGeometry` of `LineString`s, white.
- Track: only when not hidden, a point every 2 m.
- Legend (`ScreenOverlay`): the depth bands, **NOT FOR NAVIGATION** and its first sentence in the user's
  language, the licence line -- so a screenshot carries them.
- `LookAt` over the whole map, north up.
- The document's description and `ExtendedData`: notice, licence with link, author, contact, survey
  dates as allowed, depth reference, grid, detail, software.

**GeoTIFF** (QGIS and other GIS): WGS 84 lat/lon (EPSG:4326, pixel is area), 256 px tiles, deflate,
classic TIFF (over 4 GB it refuses).

- `_mosaic.tif`: 8-bit, the view's palette as the colour map, `GDAL_NODATA 0`: the raw intensity index
  stays in the file, the colours come with it.
- `_depth.tif`: float32 metres, positive down, `GDAL_NODATA -9999`, band unit `m`.
- Both: `ImageDescription` (title, notice), `Copyright` (licence line), `Software`, and the metadata as
  `GDAL_METADATA` items (TITLE, AUTHOR, CONTACT, LICENSE, LICENSE_ID, LICENSE_URL, COPYRIGHT,
  DISCLAIMER, DISCLAIMER_LOCAL, LOGGER, VERTICAL_REFERENCE, DRAFT_M, CREATED, SURVEY_FROM/TO).
- The ASCII tags hold UTF-8 (an author in Cyrillic). Pillow shows them as Latin-1 mojibake
  **[Verified]**; GDAL and QGIS are expected to show them right **[Assumption, no GDAL on this PC]**.

**metadata.json**: the field names follow IHO B-12 (Guidance to Crowdsourced Bathymetry, ed. 3.0.0,
table 1) where it has one -- provider, licence, logger, coordinateSystem, verticalReference
(`Transducer`, or `Waterline` with a draft), sensor (Sounder, KOGGER, draft), processing flags
(draftApplied, verticalReduction false, soundSpeedCorrection false) -- in a layout of this app's own, not
the CSB GeoJSON format. Then survey dates, bounds, the products (mosaic resolution, source, palette,
levels; depth cell, interpolation, edge limit, soundings, min/max depth, isobath step) and the file list.
The point: a map that is passed on keeps who made it, under what terms and how; and a later community
map or a hand-over to the IHO data centre can read these fields without redoing the files **[Inference]**.

## How it is made

`src/map_export/`:

| File | What |
| --- | --- |
| `map_exporter.{h,cpp}` | QML `mapExport`: settings, the run on its own thread, progress, cancel, the debug run |
| `map_export_geo.{h,cpp}` | the lat/lon grid, the projection, the mosaic sampler, the depth surface, isobaths |
| `map_export_formats.{h,cpp}` | GeoTIFF writer, ZIP writer, indexed PNG, CRC-32 |
| `map_export_meta.{h,cpp}` | licences, the notice, metadata as JSON / GDAL items / KML description / README |
| `map_export_writer.{h,cpp}` | the files from what was gathered (pure: no app threads) |
| `map_export_config.h` | the view's mosaic settings as the data processor last passed them on |

**The mosaic** is painted again over the whole survey with the view's settings -- what the screen holds
covers only what was in view, the precompute only what it finished. `MosaicProcessor` got a sink mode
(`setSink`, `MosaicTileSink`): no data processor, no worker, every touched tile painted, tiles put into
and taken back from the sink as the hot cache does for the view. The run is `MosaicPrecomputer::run`'s:
slices of 1024 epochs, the mesh restarted past 128 K cells. Channels, source, angle offsets and the colour
table come from `DataProcessor::mapExportConfig()` (recorded in the setters that pass them to the
worker). Tiles are kept in memory: at most 8 000 (2 000 on Android, ~66 KB each); past that the export
asks for a coarser detail.

**The depth surface** is built from the 3D bottom track as `SurfaceProcessor` gets it
(`BottomTrack::data()`: sonar north, east, -distance): one point per 1 m cell with the first point's place
and the last one's depth (as `processOneCenter`), the same `delaunay::Delaunay` (centred, the super
rectangle 8x the extent), triangles longer than the isobath tool's edge limit left out, linear
interpolation at each pixel centre of a 1 m lat/lon grid (coarser past 4096 x 4096). Then the view's
extrapolation (`SurfaceProcessor::paintDiskExtrapolated`): each sounding in the track's order gives its
depth to the cells within extra width / 2 that no triangle covers -- ahead of it (along the track, the
direction from its neighbours as `dirForIndexPix`) over an earlier extrapolation, behind it only where
there is none. `mapexport_check` holds it (between the lines the triangles stay; within reach the depth
of the sounding just passed; in a gap wider than the edge limit and the reach, nothing; without extra
width, nothing outside), and two mutants -- no direction rule, extrapolation over the triangles -- are
caught **[Verified]**. The view paints its extrapolation on its own grid; the export on its 1 m grid.

**The grid and the projection.** The scene is the app's spherical azimuthal-equidistant projection about
the dataset's reference (`NED(const LLA*, const LLARef*)`, R = 6 371 000). The files are on a lat/lon
grid; each pixel's centre is projected to the scene by `Projector`, which gives the very doubles `NED()`
gives (same expressions, same order) **[Verified, bit for bit at six places]**, and the mosaic tile byte is
read there (pixel `gx = floor(n * px/m)`, tile `gx / 256`, byte `[east][north]` as `pixel()` writes it).
The inverse, `nedToLla`, divides by the same float `M_DEG_TO_RAD` the forward way multiplies by and
round-trips to ~5e-9 m; the app's own `LLA(const NED*)` divides by 180/pi and lands 4.6 to 11 cm away on
the same points **[Verified, mapexport_check]** -- left as it is, it is not this feature's.

**Isobaths**: marching squares over the depth grid's pixel centres, saddles by the cell's mean, the
segments joined along the grid edges they cross.

## Debug run (no clicks)

`KOGGER_MAP_EXPORT=<folder>` exports into it `KOGGER_MAP_EXPORT_DELAY_MS` after start (default 60 000,
for the file to open and the bottom track to settle), at `KOGGER_MAP_EXPORT_ZOOM` if set (this run only),
quits after with `KOGGER_MAP_EXPORT_QUIT=1`; `KOGGER_MAP_EXPORT_DUMP=<file>` also writes the painted tiles
in the `KOGGER_MOSAIC_DUMP` layout. Every run logs `map export:` lines (epochs, tiles, soundings,
triangles, files, times) to kogger.log.

## Checks

- `powershell -ExecutionPolicy Bypass -File tools\mapexport_check\run.ps1 [-Keep <folder>]`: 102 checks --
  the projection bit for bit, the inverse, the grid, the sampler against `pixel()`'s layout, the depth
  surface on a plane with a gap past the limit and a line run twice, isobaths on their depth and through
  saddles, the GeoTIFFs and the KMZ read back by a TIFF parser of its own and Qt's `QZipReader`, the
  notice, the licence, no track or times when hidden; the .kmap read back (passport, depths cell for cell,
  mosaic and palette), the own ZIP and GeoTIFF readers, the depth at a point (plane, gap, off the map);
  opening a received file (`copyMapFile`, `startsLikeMap`): each .kmap copied byte for byte with its
  SHA-1, a 2.5 MB file in three pieces, a log named .kmap and a 2-byte file refused with nothing written,
  a missing file and an unwritable copy saying so. 102/102 **[Verified]**. Eight mutants (degrees by
  pi/180, sampler axes swapped, depth offset, first depth kept, tie point at the south, another datum,
  track always, saddles dropped): all caught; two more for the copy (no ZIP test: 3 fail; the first piece
  not hashed: 3 fail) **[Verified, 2026-10-18]**.
- `powershell -ExecutionPolicy Bypass -File tools\qml_check\mapexport_page_check.ps1 [-Picture page.png]`:
  the real page over mocks -- rows, Save into the folder, Cancel while running, the result, "Show in
  folder", the licence note, the switches and lists; "Share the map" -- not off Android, the .kmap (not
  the KMZ) with the title "Send the map", not without a .kmap, while running or after a failure. 10/10
  **[Verified]**; a mutant sharing the first file instead of the .kmap is caught.
- `powershell -ExecutionPolicy Bypass -File tools\qml_check\mymaps_page_check.ps1 [-Picture page.png] [-Preview preview.png]`:
  My maps over a mock library -- the empty state, the cards (author or "Author not given", licence, size
  in m or km, depths), a card's switch, "Show on the map", "Remove" asking first, the depth under the boat,
  the layer switches, how to open a map (the desktop's ways; on Android from the messenger). 9/9
  **[Verified]**. `boatgui_check` 8/8 and `sysgui_check` 12/12 with the new data field in the catalog
  **[Verified]**.

On the test survey `00014_03.05.2025.KLF` (348 MB, 57 079 epochs, Yerevan lake), profile `mapexport` (a
copy of `perf`), 2026-10-18 **[Verified]**:

- 2.6 s in all at 16 cm: 102 tiles painted (79 with data) in 1.5 s; 56 716 bottom-track points ->
  4 273 soundings, 5 655 triangles at 50 m; KMZ 3.7 MB, mosaic TIFF 2.7 MB, depth TIFF 0.24 MB.
- The export's tiles against the view's at the same zoom (`mosaic_dump_compare.ps1 -Judge`, the view's
  dump from `precompute_check.ps1`): 93 tiles in both, none painted in one only, 99 of 3 858 638 pixels
  another value (at most 16 apart), PASS.
- Laid on Esri World Imagery by the slippy-map formulas (independent of the app): the isobaths fill the
  lake's southern bay, run along its western embankment and stop at the dam; the mosaic reaches past the
  water's edge by the side-scan range.
- The GeoTIFFs open in Pillow (libtiff): size, palette, pixels, tie point, GeoKeys, nodata.
- `precompute_check.ps1` on this build: PASS, 0 pixels apart (the view's painting is unchanged by the
  sink mode).

Not verified: Google Earth itself (its survey pop-up stood over the window; no click tool here); QGIS;
Android beyond opening a map from Telegram and WhatsApp on one phone (see "Android: from a messenger and
back"); the 1.1 GB survey (memory at 8 cm and finer); a survey across the antimeridian
(refused, not handled); a live survey still streaming (the export takes what is there at its start).

## Known limits

- Depths are from the transducer unless a draft is entered; no water level, tide or sound-speed
  correction -- the metadata says so. The bottom track's errors go into the map (on 00014 a few points at
  the start read 0.4 m and give a fan of isobaths by the dam) -- cleaning is the bottom-track editor's job.
- A file being opened, closed or re-read stops a running export first (`Core::stopMapExport`).
- A ping marked bad or trimmed on the echogram (`docs/echogram-editing.md`) is out of the map: its mosaic and
  depths (since the two works met, 2026-10-19) **[verified by reading]** and the track and the survey's times
  (`MapExporter::start`) **[verified: 00014 with the first 2924 pings trimmed and 1141 marked bad, the KMZ track 1573
  points against 1665, starting later; HANDOFF "Brought together"]**. Across a stretch marked bad the track is a
  straight line between the pings either side.
- The survey's dates are written only when the recording carries a calendar date (after 2000): the
  ping's time, else the GNSS fix's. 00014 has no ping time and a GNSS time near 1970 (time since
  power-on), so its map has no date -- the first build wrote "1970-01" **[Verified, fixed]**.
- Without the track and times, the map still shows where the boat was **[Inference]**: the page says so.
- My maps: no depth under the cursor (only under the boat); the mosaic lies flat at the water surface
  and covers the depths when both are on; a map is drawn in the isobath tool's theme and step and the
  mosaic tool's theme and levels, not as its maker saw it, and the scale of colours spans all the visible maps, not the live survey's range,
  so a map and the live surface over the same water can differ in colour at the same depth; the
  isobath tool's edge limit and extra width are for the live triangulation and do not touch a map;
  overlapping maps: the newest on top; a mosaic is held whole as textures (a 1 km survey at 16 cm is
  ~40 MB of GPU memory **[Inference]**), large ones untried.
- Not verified for My maps: the head unit (GLES shaders of the same programs), the drawing on Android
  (the hand-over from a messenger is verified on a phone; the map under the boat there was not looked
  at), the basemap under the layer (the test profile had no tiles).
- Android: KoggerApp is offered for every file a messenger opens as `application/octet-stream` or a ZIP;
  anything but a KOGGER map is refused with a message. A KLF log sent that way is refused too: opening
  logs from a messenger is not built. Only the last save's .kmap has "Share the map"; maps in My maps have
  none (another's licence may not allow passing it on). The map is read whole into memory when opened, as
  on the desktop.

## Licences and the law

The page offers three licences and states what each allows; the notice is always written. The user is
the one who licenses: by default nothing is granted beyond looking at the map. Facts behind the choice
(sources read 2026-10-18):

- Raw soundings and machine-made grids or mosaics are likely not works under Armenian copyright law
  (HO-142-N art. 4: results of technical means without creative activity are not protected); a database
  has a maker's right (arts. 58-61, 15 years) **[Verified in an unofficial English translation of the
  2006 text; Inference for the conclusion]**.
- IHO B-12 publishes contributed crowdsourced bathymetry under CC0 1.0 only **[Verified]**; a later
  hand-over to the IHO data centre would need CC0 or the provider's agreement **[Inference]**.
- CC BY 4.0 lets anyone use the map with credit; CC0 waives everything, including for competitors;
  ODbL (not offered) would force a curated derived database open if published **[Verified, licence texts]**.
- A track tied to a person is personal data (HO-49-N art. 3); processing is not allowed where
  depersonalised data serves (art. 5) -- hence the default "without the track and times"
  **[Verified for the articles, Inference for the application]**.

Not legal advice. Open questions (licence form under Armenian law, publishing depths of Armenian
waters, GDPR for EU users, foreign restricted waters) are for Armenian counsel before any community map.
