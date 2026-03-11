# ZNC Custom Modules

A place to store and share my custom ZNC mods. More info to be added later.

## Modules

- [`highlightctx`](./modules/highlightctx/README.md) — a detached-only ZNC module that watches for highlights of your nick and saves the surrounding channel context, so you can catch up on exactly what led to the mention and what followed it
- [`ignore_drop`](./modules/ignore_drop/README.md) — filters matching sender traffic before it reaches your client, with separate always-on and detached-only ignore modes
- [`keepnick_instant`](./modules/keepnick_instant/README.md) - quickly regains your preferred nick using ISON, with more aggressive recovery than ZNC’s default `keepnick` on networks without nick registration services
- [`delayedperform`](./modules/delayedperform/README.md) - a per-network replacement for ZNC’s built-in `perform` module that staggers post-connect commands over time instead of firing them all at once, reducing the risk of server-side throttling.
