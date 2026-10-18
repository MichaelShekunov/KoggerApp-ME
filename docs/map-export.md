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
   Open a map). It goes into **My maps** and lies **under their boat** in the 3D view: depth colours and
   isobaths, the mosaic if they want it; the depth under the boat by the map is a data field.
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
| Side-scan mosaic, Mosaic detail | the mosaic at 2, 4, 8, 16 (default) or 32 cm a pixel -- the view's zoom levels 2..6 | yes |
| Depths and isobaths | the depth surface, its colours and isobaths | yes |
| Join soundings up to, m | the longest triangle edge of the depth surface (default 50) | yes |
| Isobaths every | Auto (10..25 isobaths over the survey's depths) or 0.5..10 m; also the colour step | yes |
| Transducer depth, m | added to every depth; > 0 makes the depths count from the waterline | yes |
| KOGGER map (.kmap) | the file to share, for KoggerApp; on by default | yes |
| For other programs: Google Earth (KMZ), GIS (GeoTIFF) | off by default | yes |
| Without the track and times | on by default: no track, the survey's date as a month | yes |

Settings live in QSettings group `mapExport` (`src/map_export/map_exporter.cpp`).

## Using it: opening (My maps)

- **Open**: the main Open dialog ("Logs and maps", or "KOGGER maps"), dropping a .kmap on the window, the
  command line (`KoggerApp.exe map.kmap`), or My maps -> Open a map. Every path ends in
  `MapLibrary::importFile`; `Core::openLogFile` hands a .kmap over before it closes anything
  (`Core::openMapFileInstead`), so opening a map never drops the survey or the device link.
- **Kept**: the file is copied into `AppDataLocation/maps` (`%APPDATA%\KOGGER\<app>\maps` on Windows) as
  `<title>_<sha1-10>.kmap`, then read from the copy: the same file twice is one map; a messenger's
  `content://` file is read once front to back and survives its cache. A map just opened is flown to
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

## The KOGGER map file (.kmap)

`src/map_export/map_package.h`. A ZIP archive, every entry **stored** (the app reads it with its own
`ZipReader`; Qt has no public raw-inflate):

| Entry | What |
| --- | --- |
| `map.json` | the passport: the fields of `metadata.json` below with `"format": "KOGGER map"`, `formatVersion` 1 (a newer one is refused with a message), `products.bathymetry.paletteTheme` the maker's depth colours |
| `depth.tif` | the GIS export's depth GeoTIFF (float32, -9999 none) |
| `mosaic.tif` | the GIS export's mosaic GeoTIFF (8-bit index, the maker's palette, 0 none) |
| `preview.png` | up to 512 px: the depth colours, or the mosaic when there are no depths |
| `README.txt` | the notice and the licence in plain words |

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
  Colours: `IsobathUtils::colorPalette(maker's theme)` spread over the bands, step Auto.
- Mosaic: the `mosaic` program, R8 index textures with mipmaps as SurfaceView's tiles, the maker's
  256-colour table, at z = 0 (the water surface): with both on, the mosaic covers the depths.
- Where the view's frame is the dataset's (near the survey, perspective), the map and the boat use the same
  formula, so they agree **[Verified: the depth map of 00014 under its own bottom track and start loop]**.

**Verified on 00014 (2026-10-18)**: a fresh profile (`receiver`) opened the .kmap from the command line --
"map library: Opened", flown to, the depths drawn with no survey open; then the recording: the map under
the boat, the bottom track on it; the mosaic layer alone: the file's mosaic under the boat matching what the
app paints itself.

The edge limit and the isobath step are the export's own, not the view's: the owner's view had 20 m and
0.1 m, which on the test survey (lines ~30 m apart) left the depth map in patches and drew isobaths 10 cm
apart -- right for a screen look, wrong for a map to share **[Verified, 00014, 2026-10-18]**.

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
rectangle 8x the extent), triangles longer than "Join soundings up to" left out, linear interpolation at
each pixel centre of a 1 m lat/lon grid (coarser past 4096 x 4096). The view's extrapolated strips
(`extraWidth`) are not exported: they are not measured.

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

- `powershell -ExecutionPolicy Bypass -File tools\mapexport_check\run.ps1 [-Keep <folder>]`: 95 checks --
  the projection bit for bit, the inverse, the grid, the sampler against `pixel()`'s layout, the depth
  surface on a plane with a gap past the limit and a line run twice, isobaths on their depth and through
  saddles, the GeoTIFFs and the KMZ read back by a TIFF parser of its own and Qt's `QZipReader`, the
  notice, the licence, no track or times when hidden; the .kmap read back (passport, depths cell for cell,
  mosaic and palette), the own ZIP and GeoTIFF readers, the depth at a point (plane, gap, off the map).
  95/95 **[Verified]**. Eight mutants (degrees by
  pi/180, sampler axes swapped, depth offset, first depth kept, tie point at the south, another datum,
  track always, saddles dropped): all caught.
- `powershell -ExecutionPolicy Bypass -File tools\qml_check\mapexport_page_check.ps1 [-Picture page.png]`:
  the real page over mocks -- rows, Save into the folder, Cancel while running, the result, "Show in
  folder", the licence note, the switches and lists. 9/9 **[Verified]**.
- `powershell -ExecutionPolicy Bypass -File tools\qml_check\mymaps_page_check.ps1 [-Picture page.png] [-Preview preview.png]`:
  My maps over a mock library -- the empty state, the cards (author or "Author not given", licence, size
  in m or km, depths), a card's switch, "Show on the map", "Remove" asking first, the depth under the boat,
  the layer switches. 8/8 **[Verified]**. `boatgui_check` 8/8 and `sysgui_check` 12/12 with the new data
  field in the catalog **[Verified]**.

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
Android (not built); the 1.1 GB survey (memory at 8 cm and finer); a survey across the antimeridian
(refused, not handled); a live survey still streaming (the export takes what is there at its start).

## Known limits

- Depths are from the transducer unless a draft is entered; no water level, tide or sound-speed
  correction -- the metadata says so. The bottom track's errors go into the map (on 00014 a few points at
  the start read 0.4 m and give a fan of isobaths by the dam) -- cleaning is the bottom-track editor's job.
- A file being opened, closed or re-read stops a running export first (`Core::stopMapExport`).
- The survey's dates are written only when the recording carries a calendar date (after 2000): the
  ping's time, else the GNSS fix's. 00014 has no ping time and a GNSS time near 1970 (time since
  power-on), so its map has no date -- the first build wrote "1970-01" **[Verified, fixed]**.
- Without the track and times, the map still shows where the boat was **[Inference]**: the page says so.
- My maps: no isobath labels yet; no depth under the cursor (only under the boat); the mosaic lies flat
  at the water surface and covers the depths when both are on; a map is drawn in its maker's colours;
  overlapping maps: the newest on top; a mosaic is held whole as textures (a 1 km survey at 16 cm is
  ~40 MB of GPU memory **[Inference]**), large ones untried.
- Not verified for My maps: the head unit (GLES shaders of the same programs), Android (opening from a
  messenger needs VIEW/SEND intent filters and a JNI hand-over, not built), the basemap under the layer
  (the test profile had no tiles).
- Android: the folder is the Export group's; there is no "Share" button yet (the FileProvider
  `${applicationId}.qtprovider` is declared and could back an `ACTION_SEND`) **[Inference]**.

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
