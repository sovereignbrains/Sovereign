# Flags

`flags.png` is a sprite of the country flags from [flag-icons](https://github.com/lipis/flag-icons)
7.5.0 (MIT, see `LICENSE`): every two-letter code's 4x3 SVG rendered to 48x36 with
`rsvg-convert`, sixteen a row, in the order of the codes (sorted). The tray draws the
exit IP's country with it (`src/tray/flags.h` lists the codes).

Rebuilt with:

```sh
npm pack flag-icons@7.5.0 && tar xzf flag-icons-7.5.0.tgz && mkdir png
codes=$(ls package/flags/4x3 | sed 's/\.svg$//' | grep -E '^[a-z]{2}$' | sort)
for c in $codes; do rsvg-convert -w 48 -h 36 package/flags/4x3/$c.svg -o png/$c.png; done
montage $(for c in $codes; do echo png/$c.png; done) -tile 16x -geometry 48x36+0+0 -background none PNG32:flags.png
```
