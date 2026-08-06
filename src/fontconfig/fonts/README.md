# Runtime fonts (`<java.home>/lib/fonts`)

These are the fonts `tools/package.sh` copies into the release runtime.

They must keep exactly these file names. `sun.font.FontUtilities` decides
`isOpenJDK` from whether `lib/fonts/LucidaSansRegular.ttf` exists; when it does
not, `SunFontManager.getDefaultFontFile()` stays `null`, and that is the file
`sun.awt.FontConfiguration.get2DCompositeFontInfo()` builds *every* logical font
(Dialog, SansSerif, Serif, Monospaced, DialogInput) on. Shipping fonts under any
other name — as the DejaVu pair did — leaves the runtime with no usable font.

`../fontconfig.properties` maps the logical fonts onto these files through the
`$JRE_LIB_FONTS` prefix, which `MFontConfiguration` expands to this directory.

| File | Source file in upstream repo |
| --- | --- |
| `LucidaSansRegular.ttf` | `Lucida Sans/Lucida Sans-Regular.ttf` |
| `LucidaSansDemiBold.ttf` | `Lucida Sans/Lucida Sans-Deml Bold.ttf` |
| `LucidaSansOblique.ttf` | `Lucida Sans/Lucida Sans-Italic.ttf` |
| `LucidaSansDemiOblique.ttf` | `Lucida Sans/Lucida Sans-Deml Bold Italic.ttf` |
| `LucidaTypewriterRegular.ttf` | `Lucida Sans Typewriter/Lucida Sans Typewriter-Regular.ttf` |
| `LucidaTypewriterBold.ttf` | `Lucida Sans Typewriter/Lucida Sans Typewriter-Bold.ttf` |

Fetched from <https://github.com/witt-bit/lucida-fonts>, renamed to the JRE file
names. Lucida Bright is not in that collection, so Serif is mapped onto Lucida
Sans for now.

The Lucida typefaces are © Bigelow & Holmes Inc. (the same faces the Oracle JDK
shipped in `jre/lib/fonts`); Lucida is a B&H trademark. The MIT licence on the
upstream repository covers that repository, not the typeface data — check the
licensing before using this in a redistributed build.
