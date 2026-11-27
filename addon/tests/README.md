# Offline addon tests

`run_tests.lua` loads the addon from `GuildAdventure.toc` into a WoW 3.3.5a API stub
(`wowstub.lua`) and drives it: codec and chunking, every vector in
`docs/protocol-vectors.txt`, the handshake against no server, an echoing realm and a scripted
wire server, a warm-relog swap over the wire (`CONTROL_PENDING`, swap-target CVar, window
reopened on the new character), a world-reload swap (pushes from the new character dropped
until the client is that character, then a new handshake), the key bindings, then demo mode
end to end through the UI
(tabs, roster, orders with the zone picker, Autopilot limits, professions, recruiting, take
control / return, bench, buying an unlock, the simulation, "While you were away", leaving demo
mode), and finally a check for stray globals.

The stub only knows widget methods, templates, font objects, anchor points and script names
that exist in the 3.3.5a client (checked against its FrameXML), so code that would fail in the
client because an API is missing fails here too. It does not render anything: layout and looks
still need a look in the real client.

## Run

Any Lua 5.1 or 5.2 interpreter works (the stub removes 5.2-only globals):

```powershell
.\addon\tests\run.ps1 -Lua C:\path\to\lua.exe
```

or directly:

```
lua addon/tests/run_tests.lua addon/tests
```

Optional environment variables:

| Variable | Effect |
|---|---|
| `GA_FRAMEXML` | folder with the client's `Interface\FrameXML` files; the real `UIPanelTemplates.lua` (tabs, faux scroll frames) then replaces the stub fallbacks |
| `GA_TEXTURES_OUT` | file to write every texture path the UI used, e.g. to check them against the client MPQs |

The FrameXML files are Blizzard's and are not part of this repository; extract them from the
client's `locale-enUS.MPQ` / `patch-enUS*.MPQ` if you want that mode.
