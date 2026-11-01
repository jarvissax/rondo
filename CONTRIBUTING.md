# Contributing

Thanks for wanting to help. Bug reports, fixes and ideas are all welcome.

- **Bugs:** [open an issue](https://github.com/jarvissax/rondo/issues) with your Y1's wheel type
  (A or B), what you did, what happened, and Rondo's log if you can
  (`adb logcat -d -v time -s Y1`).
- **Code:** [Building Rondo](docs/building.md) explains the layout, the tools and the rules the
  code follows. Test app changes on a real Y1; there's no emulator for the wheel. For the
  server, run `npm test` and `npm run typecheck` in `server/`.
- **Pull requests:** keep them focused on one thing, and say how you tested it.
- **Never commit secrets:** cookies.txt, tokens, `.env` files or signing keys. `.gitignore`
  covers the usual names.

By contributing, you agree that your contribution is released under the project's
[MIT License](LICENSE).
