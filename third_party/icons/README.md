# Icons of the interface (resources/icons/ui)

Where the interface's icons come from and under what licence; the files are compiled into the
application's resources (`resources/icons.qrc`). Written 2026-10-19 from the files themselves: 209 SVG
and 2 PNG files then. **[verified]** is read from the files or the upstream licence, **[inference]**
and **[assumption]** are not.

| Set | Files | Licence | Text |
|---|---|---|---|
| [Tabler Icons](https://tabler.io/icons) (<https://github.com/tabler/tabler-icons>) | the SVGs whose `class` attribute has `icon-tabler` (196) **[verified]** | MIT, Copyright (c) 2020-2026 Paweł Kuna **[verified, the upstream LICENSE]** | [tabler-icons-LICENSE.txt](tabler-icons-LICENSE.txt) |
| [Ionicons](https://ionic.io/ionicons) 5 (<https://github.com/ionic-team/ionicons>) | `code-working.svg`, `settings.svg`, `settings-outline.svg`: their `<title>` is `ionicons-v5-...` **[verified]** | MIT, Copyright (c) 2015-present Ionic **[verified, the upstream LICENSE]** | [ionicons-LICENSE.txt](ionicons-LICENSE.txt) |
| "Zoom fit" (`dark_zoom-fit.svg`) by Timothée Giet, 2016 | `fit-in-view.svg` | CC BY-SA 4.0, as its metadata states **[verified]**; from Krita's icon set **[inference: its name and author]** | [zoom-fit-ATTRIBUTION.txt](zoom-fit-ATTRIBUTION.txt) |
| Downloaded from SVG Repo (<https://www.svgrepo.com>; each icon is under the licence of its collection there) | `3dcube.svg`, `combo-selection.svg`, `one-finger.svg`, `tool.svg`: "Uploaded to: SVG Repo" in the file **[verified]** | **not known**: the collections were not recorded | -- |
| Traced with Potrace | `double_route.svg`, `double_route_crossed_out.svg`, `isobaths.svg`, `side_scan.svg` | the project's own drawings **[assumption]** | -- |
| Not recorded | `terminal.svg` (two strokes), `conn.png`, `diagonal_stripe.png` | the project's own **[assumption]** | -- |

The Tabler files were copied in over time and their version was not recorded: the newer carry the class
`icons-tabler-outline` of Tabler 3 **[inference]**, the older not. The licence texts above ship inside
the application (`resources/resources.qrc`, `licenses/...`) and are listed on *Settings -> Interface ->
About* (`qml/app/AboutPage.qml`); `THIRD_PARTY_NOTICES.md` names the three sets.

**Open:** the four SVG Repo icons -- find each on svgrepo.com and its collection's licence (and credit it
here if it asks for that), or draw them anew / take Tabler ones.

**Adding an icon.** Prefer Tabler's outline set (24 x 24, `stroke="currentColor"`): take the SVG from
tabler.io as it is and keep its `class` attribute, which is how `tools\qml_check\about_check.ps1` tells a
Tabler file. An icon from anywhere else gets a row here with its source and licence -- and, when its
licence asks for a notice, the text in this folder, an alias in `resources/resources.qrc`, an entry in
`qml/app/AboutPage.qml` and a row in `THIRD_PARTY_NOTICES.md`. The check fails on an SVG that is neither
Tabler nor named in this file.
