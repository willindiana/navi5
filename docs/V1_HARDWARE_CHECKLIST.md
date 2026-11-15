# v1.0.0 hardware checklist

Host tests and native builds do not show how any of this behaves on a TV, with
a real decoder, or against a real server; this list does.

Run the whole list on the **DEV** title first (`NAVD00001`, default build),
then the marked items again on a **`--release`** build (`NAVI00001`, Media).
Deploy rules are in [DEVELOPMENT.md](DEVELOPMENT.md): close the app with the PS
button first, DEV only unless `--prod` is separately decided, never `PPSA99505`.
Use a disposable account and a server you can reset.

Record for each run: commit, build mode, artifact SHA-256, firmware, loader,
Navidrome version, HTTP or HTTPS, and pass / fail / not run per line.

`R` = repeat on the `--release` build. Media builds blank Remote Play: check
those on the TV.

## 1. Login screen

- [ ] R Fresh launch, no saved login, one private address: the form (server,
  username, password, show password, Connect) is on the left with focus on the
  server row; the QR shows on the right without pressing anything; one status
  line at the bottom; no paragraph text.
- [ ] R Scan the QR, tick the trusted-LAN box, sign in: app connects, even with
  half-typed values in the form.
- [ ] QR code is scannable from the sofa on the TV (size, contrast, safe area).
- [ ] Two or three private addresses (e.g. Ethernet plus Wi-Fi): the picker
  shows in the QR area and has focus; Up/Down choose, Cross starts, the three
  warning lines are readable and not clipped.
- [ ] No private address (network unplugged or public-only): no QR area; the
  form is centered and works once the network is back.
- [ ] Right moves focus to the QR area, Left back to the same row; Circle on the
  QR area stops pairing ("QR pairing is stopped"), Cross there starts a new code.
- [ ] R Form: each row opens its own keyboard; show/hide password works; Cross
  on Connect connects.
- [ ] Cancel the keyboard on a row: earlier values are kept.
- [ ] Server typed without `http://`/`https://`: protocol choice appears.
- [ ] R HTTP warning: choose HTTP (or type `http://…`): the warning appears and
  nothing is sent before Cross; Circle cancels and nothing connects. After
  accepting, the status line says HTTP is unencrypted.
- [ ] Wrong password: back on the login screen with the reason in the status
  line and a **new** QR code.
- [ ] Circle while "Connecting …": returns to the login screen, QR restarts.

## 2. QR and TLS lifecycle

- [ ] Leave the QR untouched: it expires at 300 s, restarts by itself twice,
  then shows "Pairing expired", focus moves to the QR area, Cross gives a new code.
- [ ] Phone form: five wrong attempts consume the code; the old link stops working.
- [ ] A link from an expired or replaced code is refused.
- [ ] Starting a manual connect while the QR is showing makes the phone page
  stop answering (listener closed).
- [ ] After logout the QR shown is a new code; the pre-logout link is dead.
- [ ] HTTPS server with a valid certificate connects; wrong hostname, expired
  and untrusted certificates fail with a certificate message and no downgrade;
  a redirecting address is refused.
- [ ] HTTPS key issuance on hardware (still unverified from the beta): password
  sign-in to an HTTPS Navidrome with API keys creates a `Navi5 (PS5)` player.

## 3. Logout

- [ ] R The gear at the far right of the header (Right past Search) opens the
  same menu with Cross, and it drops down beneath the gear.
- [ ] R While a track plays, Options opens a small menu with "Log out"; Circle
  or Options closes it and playback is untouched.
- [ ] R Cross, then the confirmation: with a saved API key the text says the
  key stays valid until the `Navi5 (PS5)` player is deleted in Navidrome; with a
  saved password that sentence is absent. Circle goes back.
- [ ] R Confirm: audio stops at once, the login screen appears with a QR, the
  status line confirms the saved login was forgotten.
- [ ] R Close and relaunch: login screen, no auto-connect.
- [ ] After logout the server and username fields are empty.
- [ ] Log out immediately after signing in (within a second or two): still no
  auto-connect on relaunch.
- [ ] Log out during an artist Play Next that is still loading: no music starts
  afterwards.
- [ ] In Navidrome the old key still works until the player is deleted (as the
  text says), and stops after.

## 4. Saved login failed

- [ ] Server off, launch with a saved login: "Saved login failed" with Retry
  and "Forget saved login"; reason in the status line.
- [ ] Retry with the server back: connects.
- [ ] Revoked key (delete the player in Navidrome), launch: same screen with
  the invalid-key reason. Circle opens the login screen; file is kept (relaunch
  shows the failure again).
- [ ] Forget saved login: login screen with QR; relaunch shows the login screen.
- [ ] The normal login list has no "Forget saved login".

## 5. Play Next (Triangle)

For each: the confirmation appears top right ("Playing next: <title>" or
"Playing next: N tracks") and the hint row shows Triangle "Play next".

- [ ] R Track while playing: it is next (Up Next card near the end shows it).
- [ ] Three tracks in a row: they play in the order added, after the current one.
- [ ] R Album while playing (focus an album card, or the Play button on its
  page): all tracks, in album order, after the earlier Play Next items.
- [ ] Playlist the same way.
- [ ] R Artist (Play button on the artist page): "Loading tracks …", then the
  count; music keeps playing without a stutter while it loads.
- [ ] Large artist during playback: the current track still hands over
  gaplessly to its successor while the artist is loading.
- [ ] While paused: item is queued, playback stays paused; resume plays it next.
- [ ] R While stopped (nothing playing): the item starts playing.
- [ ] After the last track finished: the item starts playing.
- [ ] Search result (track, album, artist): Triangle queues it; Square still
  deletes a search character and does not pause.
- [ ] Triangle on tabs, the "More" row or an empty/error page does nothing.
- [ ] **Gapless boundary.** Play a gapless album. Press Triangle on another
  track about 15 s before the end of the current one: the new track follows
  with no cut-off and the old successor is not heard first.
- [ ] Press Triangle in the last 1 to 2 s of a track, several times across
  attempts: either the new track is next, or the already-started successor
  plays and the new track follows it. Never silence, a repeated track, a
  skipped track or a title that does not match the audio.
- [ ] Add a second item (successor unchanged) near a boundary: transition stays
  gapless.
- [ ] Next right after Triangle goes to the just-added track.
- [ ] Seek, then Triangle straight away: applies once the seek settles.

## 6. Queue view

- [ ] R Fullscreen player: Right to the Queue control (named "Queue" when
  focused), Cross opens the list with the current track first, marked "Now playing".
- [ ] Cross on an upcoming track picks it up (row is inset), Up/Down moves it,
  Cross drops it; the order holds and plays that way.
- [ ] R Move a track into the **immediate successor** slot, let the current
  track end: the moved track plays next, gaplessly.
- [ ] Move the immediate successor away: the following track plays next.
- [ ] Square removes the focused track; removing the immediate successor makes
  the one after it play next.
- [ ] The current track cannot be picked up or removed; nothing can be carried
  above it.
- [ ] Circle while carrying cancels the carry; Circle again closes the view.
- [ ] Track change while the view is open: list advances, focus stays on its track.
- [ ] Track change while carrying a track: nothing wrong is moved.
- [ ] Reorder or remove right at a track boundary: "Updating queue …" may show
  briefly; result matches what was asked, audio continuous.
- [ ] Artist Play all with thousands of tracks: the view opens at once, scrolls
  smoothly held down, L1/R1 page; carry a track a long way and drop it.
- [ ] Stop while the view is open closes it.

## 7. Shuffle with Play Next

- [ ] R Shuffle on, Triangle on a track: it still plays next.
- [ ] Two Play Next items, then toggle shuffle on: both still play next, in order.
- [ ] Toggle shuffle off: same.
- [ ] Shuffle on, reorder in the queue view: order holds; shuffle off restores
  album order for what is left.

## 8. Buttons

- [ ] R Touchpad opens the fullscreen player from browsing; it never stops playback.
- [ ] Touchpad with nothing loaded does nothing.
- [ ] R Stop is in the fullscreen control row (named "Stop" when focused); Cross
  stops and returns to browsing.
- [ ] Triangle no longer opens the player or jumps to the header.
- [ ] R Options opens the menu from browse, fullscreen and queue view.
- [ ] Square pauses/resumes in browse and fullscreen, backspaces in Search,
  removes in the queue view.
- [ ] Media remote, if you have one: Play/Pause and Options behave the same.
- [ ] Every on-screen hint names the button that actually does it (compact bar
  shows Touchpad; footers show Triangle only where it works).

## 9. Seek

- [ ] R Down to the seek bar, Left/Right preview, Cross applies, Circle cancels.
- [ ] Seek with Play Next items waiting; seek, then open the queue view and edit.
- [ ] Left/Right across the six controls and Down/Up to the seek bar feel right.

## 10. TV safe area and long strings

- [ ] R On the TV (not Remote Play): login panel, QR, list, status line, queue
  view, logout confirmation and the top-right confirmation are inside the safe
  area, nothing clipped.
- [ ] Switch the console language to German, French, Finnish, Japanese and
  Arabic: "Enter server manually", the logout confirmation (six lines max), the
  Play Next confirmation, queue hints and the picker warnings fit or truncate
  cleanly, no overlap.
- [ ] The new strings in the 25 non-English languages are machine-written and
  unreviewed; report any that read wrong.
- [ ] Very long track titles in the queue view and in the Play Next confirmation
  truncate with an ellipsis.
- [ ] The version line on the login screen reads `v1.0.0-<commit>` (DEV adds
  ` / dev`).

## 11. Release build only

- [ ] `--release`: title `NAVI00001`, Media, version 01.000.000; installs beside
  DEV; update over the beta keeps the saved login and it still connects.
- [ ] Background audio and the system media controls behave as before.
- [ ] Icon and title in the Media gallery.
