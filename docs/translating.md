# Translating Rondo

Rondo speaks English and Spanish. Every text it shows, on the Y1, on its dashboard, on the
server's admin page and on the update page, lives in one file per language in [`lang/`](../lang):

| File | Language |
| --- | --- |
| [`lang/en.json`](../lang/en.json) | English: the source of every text, and what Rondo falls back to |
| [`lang/es.json`](../lang/es.json) | Español |

Fixing a translation is a one-file pull request. Adding a language is one new file. You don't need
a Y1 or a build: a check script tells you when something doesn't fit, and the dashboard and the
admin page run on your computer with your changes.

## What a language file looks like

```json
{
  "_name": "Español",
  "device": {
    "settings.language": "Idioma",
    "settings.theme.menuShown": "{shown} de {total} visibles",
    "library.songCount": { "one": "{count} canción", "other": "{count} canciones" }
  },
  "dashboard": { "…": "…" },
  "admin": { "…": "…" },
  "updater": { "…": "…" }
}
```

- **`_name`** is the language's name in that language ("Español", "Deutsch"). The language pickers
  show it.
- **Four sections**, one for each place Rondo shows text: `device` (the Y1 itself), `dashboard`
  (the web page the Y1 serves while Settings › Dashboard is open), `admin` (the Rondo server's
  admin page) and `updater` (the update page,
  [jarvissax.github.io/rondo](https://jarvissax.github.io/rondo/)).
- **Keys** (`settings.language`) name a text and where it's used. They never change in a
  translation; only the text on the right does.
- **Blanks** in braces, like `{count}`, `{name}` or `{total}`, are filled in by Rondo. Keep each
  blank the English has, spelled the same; move them wherever your language needs them
  (`"{month} {day}"` in English is `"{day} {month}"` in Spanish).
- **Plural forms**: a text with a number in it can have one text per plural form, chosen by
  `{count}`. English and Spanish use `one` and `other`; your language may need others from
  [CLDR's list](https://www.unicode.org/cldr/charts/latest/supplemental/language_plural_rules.html)
  (`zero`, `two`, `few`, `many`). Every plural text needs `other`. A form may leave `{count}` out
  (`"one": "Una canción"`).
- **Markup**: a few dashboard, admin and update page texts have `<b>…</b>`, `<i>`, `<code>` or
  `<br>` in them. Keep the same tags, around the words that need them in your language.
- `\n` is a line break (a few texts on the Y1 have two lines). `…` is one character (the ellipsis),
  as are `’`, `“`, `”`, `·` and `›`.

Anything a language doesn't have yet shows in English, so a half-finished language still works.

## Fixing a translation

1. Find the text in your language's file: search it for the words you saw (or search
   `lang/en.json` for the English, then the key in your language's file).
2. Change the text on the right of the key. Keep the blanks and the markup.
3. Run the check (needs [Node.js](https://nodejs.org) 22 or newer, nothing else):

   ```bash
   node tools/lang/check.mjs
   ```

4. Open a pull request with that one file.

## Adding a language

1. Copy `lang/en.json` to `lang/<code>.json`, named by the language's code: `de`, `fr`, `tr`, or with
   its region where it matters (`pt-BR`, `zh-TW`).
2. Set `_name` to the language's own name, then translate every text. Start with the `device`
   section: it's what people see first.
3. Run `node tools/lang/check.mjs` until it has nothing to say. It tells you about a missing text, a
   blank that doesn't match English, a key English doesn't have, or broken JSON.
4. Open a pull request. The new language shows in every picker by itself.

Two things need a developer, so mention them in the pull request if your language has them:

- **Plural forms besides `one` and `other`.** The dashboard, the admin page and the update page
  take the rules from the browser. The Y1 has its own table in `native/core/lang.cpp`
  (`pluralForm()`); it already knows French, Russian, Ukrainian, Polish, Czech, Slovak and the
  languages without plurals.
- **Writing that runs right to left** (Arabic, Persian, Hebrew). The Y1 lays text out left to right
  for now.

The Y1's font is Roboto, with the Y1's own fonts for Chinese, Japanese, Korean, Arabic and others.

## Seeing it without a Y1

The dashboard and the admin page run on your computer with mock servers that read `lang/` on every
reload, so an edit shows the moment you reload the page.

**The dashboard**, with the Rondo set to your language:

```bash
node tools/dashboard-mock/server.mjs --lang es
```

Open <http://localhost:8090> and type the pairing code the terminal prints. The Settings page has the
Language choice, as on the Y1.

**The admin page**:

```bash
node tools/admin-mock/server.mjs
```

Open <http://localhost:8095/admin/> and choose the language under the sign-in form (or, signed in
with the password the terminal prints, under Admin account). It starts in your browser's language
when Rondo has it.

**The update page** (needs `npm ci` in `web-updater/` once):

```bash
cd web-updater && npm run dev
```

Open <http://localhost:8000> and choose the language at the top right. An edit in `lang/` rebuilds
the page; reload to see it.

**The Y1's own texts** need a Y1 to see in place (Settings › Language). Its screen is 480 × 360 and
its rows are short: a text that doesn't fit ends in "…", so prefer the shorter way of saying it,
especially for titles (about 22 characters) and the values at the right of a row (about 12).

## For developers: adding a text

Every text goes through the catalog; none is written in the code.

- **The Y1** (C++): `tr("settings.language")`, `tr("settings.theme.menuShown", {{"shown", n},
  {"total", m}})`, `tr("library.songCount", {{"count", n}})` (`native/core/lang.h`). Call it where
  the text is shown, never in a static initializer. `native/services`, `native/player` and
  `native/direct` run in processes of their own and don't translate: their texts are translated
  where the shell shows them. The few words direct mode and the Rondo server put into YouTube's
  answers themselves (`feed.*`: "49 songs", "Liked Music") are in the `device` section too, said in
  the language each request names (`say()` in `native/direct/innertube.h` and `server/src/lang.ts`).
- **The dashboard, the admin page and the update page** (JavaScript): `t('key', {count: n})` for
  text, `te('key')` inside HTML templates (escaped), `th('key')` for a text with its own markup, and
  `data-i18n="key"`, `data-i18n-html="key"`, `data-i18n-attr="aria-label:key"` in the HTML (keep the
  English in the HTML too; the check makes sure it matches `lang/en.json`). The update page's texts
  are built into its script (`web-updater/build.mjs`), so it fetches none.
- Add the English to `lang/en.json` and the text to every other language file. Build sentences as
  one text with blanks, never from pieces (`"{count} songs"`, not `n + " songs"`): word order
  differs between languages.
- `node tools/lang/check.mjs` runs in CI and in the server's tests; `--format` sorts English's keys
  and puts every language in English's order, `--unused` lists English texts no code names.

## Spanish

Spanish is neutral Spanish, with *tú*, and the words Rondo uses for its own things:

| English | Español | | English | Español |
| --- | --- | --- | --- | --- |
| Settings | Ajustes | | Playlists | Listas |
| Now Playing | Reproduciendo | | Shuffle | Aleatorio |
| Library | Biblioteca | | Queue | Cola |
| Songs | Canciones | | Lyrics | Letra |
| Albums | Álbumes | | Downloads | Descargas |
| the card | la tarjeta | | Dashboard | Panel |
| Headphones | Auriculares | | Speaker | Altavoz |
| Sign in | Iniciar sesión | | Pair | Vincular |
