# Tiny Bar Casino

A roulette table that runs on a Cheap Yellow Display (ESP32-2432S028R), 320x240
landscape. One file: [uhhhcasino.ino](uhhhcasino.ino).

Needs the Adafruit GFX Library, Adafruit ILI9341, and XPT2046_Touchscreen. Pins
are defined at the top of the sketch, not in a library config file.

## The wheel

38 pockets, American odds, but laid out European-style: `0` anchors the left end
of the board and a second zero — called `38` here, and colored gold so it doesn't
read as a twin — anchors the right end. Both pay 35:1 straight up, and both kill
every outside bet.

## Playing

You log in first. Type a name on the on-screen keyboard; a new name gets you a
PIN of your choosing and $1,000, a known name asks for the PIN you already set.
Up to 12 accounts live in the board's flash, so wallets and history survive being
unplugged.

At the table you pick a chip ($1 up to $10K, `-`/`+`), tap numbers, dozens, or
the even-money bets to stack chips on them, and hit SPIN. `REBET` puts your last
spread back down and `2X` doubles whatever's on the table — both all-or-nothing,
so they refuse rather than half-fill a bet you can't cover. The spin runs about
seven seconds and lands somewhere different every time.

Tapping your bankroll in the header opens the leaderboard, and MY ACCOUNT from
there is where the settings live.

## Money

Your wallet is play money and Gabe keeps refilling it. Go broke and he hands you
a fresh stake; cash out while you're up and he hands you another one. The tier
scales with your biggest single cash-out ever — $1K normally, $5K past a $100K
cash-out, $25K past a million — and going broke never demotes you.

The leaderboard number is the honest one: everything you've ever cashed out, plus
what's in your wallet right now, minus every dollar Gabe has ever given you. A
refill lands in both halves of that at once, so it cancels out and can't be
mistaken for a win. The house's own running total sits under the divider.

## Settings

On the account screen:

- **Vol** — OFF / LOW / MED / HIGH (duty cycle on the speaker, not a real amp)
- **View** — STRIP (pockets rip past a marker) or WHEEL (ball orbits a ring)
- **Lightning** — Lightning Roulette rules: each spin strikes 1–5 numbers with
  50x–500x multipliers. Straight-up bets drop to 29:1 to pay for it, so the house
  edge stays where it was.
- **Delete acct** — self-service, with a confirmation

All of these persist across power cycles.

## No touchscreen?

The whole game also drives from the serial monitor at 115200. Type your name and
press Enter, then your PIN and Enter. At the table:

```
w/a/s/d  move cursor      p  place chip     g  spin
c        clear bets       r  rebet          t  double
- / +    chip size        k  leaderboard    v  volume
```

The cursor stays hidden until you actually type something, so it never shows up
on real hardware.
