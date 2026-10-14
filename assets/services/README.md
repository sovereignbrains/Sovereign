# Services

A logo for each service of the routing board (`ServiceCatalog` in `src/tray/routing.cpp`),
`<id>.svg`, a resource `SVC_<ID>` of the tray (`src/tray/tray.rc`). `icons.cpp` draws them with
Direct2D's SVG for the menus and the board, at the size wanted - so only what it draws: paths,
shapes, a fill or a stroke, no `<use>`, filters or masks. Colors for a dark background (the
menus are always dark): a black logo is white here.

- [Simple Icons](https://github.com/simple-icons/simple-icons) 16.34.0 (CC0): netflix, youtube,
  gemini (`googlegemini`), claude, spotify, hbo, twitch, instagram, facebook, x, discord, telegram,
  whatsapp, perplexity, github, reddit, soundcloud, steam, epicgames, apple - the icon's path with
  its brand color as the fill (white for a black one).
- [theSVG](https://github.com/glincker/thesvg) (MIT, see `LICENSE`), from Iconify's
  `thesvg-color`: chatgpt (`openai-dark`), grok (`grok-dark`), linkedin, microsoft, tiktok
  (`tiktok-dark`).
- [gilbarbara/logos](https://github.com/gilbarbara/logos) (CC0), from Iconify's `logos`: google
  (`google-icon`).
- Also Simple Icons: cloudflare, quad9 - the DNS providers (google's is the one above).
- [Hysteria](https://github.com/apernet/hysteria) (MIT): hysteria - the "H" of its logo.svg (the first
  letter's polygons and the yellow stroke; the rest is lettering), white for the dark background.
- Drawn for this app: disney, primevideo - their logos are lettering, unreadable at 16 px; a badge
  in the brand's color instead. And the board's lists: lan, a router (the local network), russia, the flag (Russian sites), ads, a shield (the ads lists), cookies, a cookie (cookie windows).

Iconify's files lose their `width`/`height` (1em: the size is the viewport's). Checked by
`ui-snapshot`: every service's logo draws.
