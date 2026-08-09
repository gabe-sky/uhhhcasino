// ---------------------------------------------------------------------------
// CYD Roulette
//
// American wheel (38 pockets) with a European-style board: 0 anchors the left
// end, and the second zero -- called 38 here -- anchors the right end. Both
// are green, both pay 35:1 straight up, and both kill every outside bet.
//
// 320x240 landscape on the Cheap Yellow Display (ESP32-2432S028R).
// Libraries: Adafruit GFX Library, Adafruit ILI9341, XPT2046_Touchscreen.
// ---------------------------------------------------------------------------

#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_ILI9341.h"
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>

// ---------------------------------------------------------------- Display ---
// Pins are set here, in the sketch, not in a library config file.
#define TFT_CS   15
#define TFT_DC    2
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCK  14
#define TFT_BL   21      // backlight (real hardware; harmless in the simulator)

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC);

#define SCR_W 320
#define SCR_H 240

// ------------------------------------------------------------------ Types ---
// This must come before the first function definition. The Arduino IDE
// inserts its auto-generated prototypes right above the first function, and
// any prototype mentioning Zone fails if the type is declared further down.
struct Zone { int x, y, w, h; };

// Declared early: the sound section persists volume changes immediately,
// before the rest of the persistence layer is defined further down.
Preferences prefs;

// Which screen we're on. Spins and result screens block inline, so they
// don't need entries here.
enum GState { ST_LOGIN_NAME, ST_LOGIN_PIN, ST_NEW_PIN, ST_BET, ST_SCORES,
              ST_ACCOUNT, ST_CONFIRM_DELETE };

// ------------------------------------------------------------------ Touch ---
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define RAW_X_MIN 200
#define RAW_X_MAX 3700
#define RAW_Y_MIN 240
#define RAW_Y_MAX 3800
#define TOUCH_MIN_PRESSURE 300

SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

// A touch only counts if it has real pressure AND isn't at raw (0,0).
// A disconnected or unemulated touch chip reads back as all zeros, which the
// library reports as maximum pressure at the origin -- a permanent fake press.
bool realTouch(TS_Point &p) {
  if (!ts.touched()) return false;
  p = ts.getPoint();
  if (p.x == 0 && p.y == 0) return false;
  return p.z >= TOUCH_MIN_PRESSURE;
}

bool getTap(int &x, int &y) {
  static bool wasDown = false;
  bool isDown = ts.touched();
  bool fresh = false;
  if (isDown && !wasDown) {
    TS_Point p;
    if (realTouch(p)) {
      x = map(p.x, RAW_X_MIN, RAW_X_MAX, 0, SCR_W);
      y = map(p.y, RAW_Y_MIN, RAW_Y_MAX, 0, SCR_H);
      x = constrain(x, 0, SCR_W - 1);
      y = constrain(y, 0, SCR_H - 1);
      fresh = true;
    } else {
      isDown = false;
    }
  }
  wasDown = isDown;
  return fresh;
}

// Continues on a real screen tap or any key typed into the serial monitor.
//
// The release wait is bounded: if the touch line is stuck reporting a press
// (unemulated chip, loose ribbon cable), we give up waiting for a release
// after a moment and carry on watching for input. Without that bound, this
// function could never return and the game would freeze on the result screen.
void waitForTap() {
  TS_Point p;
  unsigned long t0 = millis();
  while (realTouch(p) && millis() - t0 < 600) delay(20);

  while (Serial.available()) Serial.read();

  for (;;) {
    if (realTouch(p)) {
      unsigned long t1 = millis();
      while (realTouch(p) && millis() - t1 < 600) delay(20);
      return;
    }
    if (Serial.available()) {
      while (Serial.available()) Serial.read();
      return;
    }
    delay(20);
  }
}

// ------------------------------------------------------- Sound and light ---
#define SPEAKER_PIN 26
#define LED_R 4          // onboard RGB LED, active LOW
#define LED_G 16
#define LED_B 17

// Volume as 4 discrete levels rather than on/off. A piezo/small speaker has
// no real amplitude control -- it's driven by a square wave that's either on
// or off -- so "volume" here means the PWM duty cycle of that square wave.
// Lower duty delivers less average energy to the speaker, which lowers
// perceived loudness on most CYD boards, but how audible the difference is
// depends on Gabe's exact speaker/amp circuit. Test on hardware and adjust
// the DUTY_* constants below if a level doesn't sound right.
#define VOL_OFF  0
#define VOL_LOW  1
#define VOL_MED  2
#define VOL_HIGH 3
#define N_VOL_LEVELS 4
const char* VOL_LABEL[N_VOL_LEVELS] = { "OFF", "LOW", "MED", "HIGH" };
const int   DUTY_AT_VOL[N_VOL_LEVELS] = { 0, 32, 96, 200 };   // out of 255

int volLevel = VOL_MED;

// Display and rules toggles, both persisted.
bool useWheel   = false;   // false = scrolling strip, true = wheel + ball
bool lightningOn = false;  // Lightning Roulette rules

// --- Lightning Roulette ---------------------------------------------------
// Each spin, a handful of numbers are struck and carry a big multiplier.
// Straight-up bets pay less than normal to fund it (see settle()).
#define MAX_STRIKES 5
uint8_t lightNum[MAX_STRIKES];
int     lightMult[MAX_STRIKES];
int     lightCount = 0;

// Multiplier table, weighted so 50x is common and 500x is rare.
const int  LIGHT_MULT[]   = {  50, 100, 150, 200, 250, 300, 400, 500 };
// Weights chosen so the average multiplier lands near 106x, which is what
// it takes for lightning's straight-up edge to match the normal game's
// 5.26% -- see the note above LIGHT_PLAIN_RETURN. An earlier draft averaged
// 94.5x and quietly made lightning WORSE for the player (7.65%).
const int  LIGHT_WEIGHT[] = {  55,  19,  10,   6,   4,   3,   2,   1 };
#define N_LIGHT_MULT 8
#define LIGHT_WEIGHT_SUM 100

// Straight-up return (total, including stake) when lightning mode is on and
// the number was NOT struck. 30 means 29:1 -- reduced from the normal 35:1,
// which is what pays for the multipliers.
#define LIGHT_PLAIN_RETURN 30

int lightMultFor(uint8_t n) {
  for (int i = 0; i < lightCount; i++) if (lightNum[i] == n) return lightMult[i];
  return 0;
}

#define TONE_RES_BITS 8    // duty is 0-255

void ledSet(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}
void ledOff() { ledSet(false, false, false); }

// Starts a square wave at `freq` with duty scaled to the current volume.
// Bypasses Arduino's tone() wrapper (which is fixed at 50% duty) so volume
// is actually adjustable.
void toneStart(int freq) {
  int duty = DUTY_AT_VOL[volLevel];
  if (duty <= 0) return;
  ledcWriteTone(SPEAKER_PIN, freq);
  ledcWrite(SPEAKER_PIN, duty);
}
void toneStop() { ledcWrite(SPEAKER_PIN, 0); }

// Non-blocking: starts the tone and returns; the caller keeps drawing while
// the ESP32's LEDC hardware keeps generating the wave. `ms` is honored by a
// short blocking wait for effects short enough that this doesn't matter, or
// left to the next toneStart()/toneStop() call to cut it off.
void blip(int freq, int ms) {
  if (volLevel == VOL_OFF) return;
  toneStart(freq);
  delay(ms);
  toneStop();
}

// Blocking, for jingles where notes must play in order.
void note(int freq, int ms) {
  if (volLevel == VOL_OFF) { delay(ms); return; }
  toneStart(freq);
  delay(ms);
  toneStop();
}

void sClick() { blip(2200, 12); }
void sChip()  { blip(1400, 18); }
void sTick()  { blip(1900,  6); }

void sSpinUp() {
  if (volLevel == VOL_OFF) return;
  for (int f = 400; f < 1600; f += 120) { toneStart(f); delay(18); }
  toneStop();
}

// Cycles OFF -> LOW -> MED -> HIGH -> OFF, plays a sample tone so the change
// is audible immediately, and persists the choice.
void cycleVolume() {
  volLevel = (volLevel + 1) % N_VOL_LEVELS;
  prefs.putInt("vol", volLevel);
  if (volLevel != VOL_OFF) { toneStart(1600); delay(90); toneStop(); }
}

void sWin() {
  note(784, 90); note(988, 90); note(1319, 160);
}

void sLose() {
  note(392, 110); note(330, 110); note(262, 200);
}

void sPush() { note(523, 80); note(523, 80); }

void sBust() {
  note(392, 180); note(370, 180); note(349, 180); note(330, 420);
}

// ----------------------------------------------------------------- Colors ---
#define C_WHITE   0xFFFF
#define C_BLACK   0x0000
#define C_GREY    0x8410
#define C_FELT    0x0304    // dark casino green
#define C_PANEL   0x18E3
#define C_GOLD    0xFE40
#define C_RED     0xC000    // number-cell red
#define C_GREEN   0x0480    // the zeros
#define C_LINE    0x52AA    // cell borders
#define C_WIN     0x07E0
#define C_LOSE    0xF800

// ------------------------------------------------------------ Text helpers ---
// Adafruit_GFX has no centered-text call, so these do the arithmetic.
// The built-in font is 6x8 pixels per character at size 1.
int textW(const char* s, int size) {
  int n = (int)strlen(s);
  return n * 6 * size - size;      // no trailing gap after the last char
}
int textH(int size) { return 8 * size; }

void textAt(const char* s, int x, int y, int size, uint16_t fg) {
  tft.setTextSize(size);
  tft.setTextColor(fg);
  tft.setCursor(x, y);
  tft.print(s);
}

void textC(const char* s, int cx, int cy, int size, uint16_t fg) {
  textAt(s, cx - textW(s, size) / 2, cy - textH(size) / 2, size, fg);
}

void textR(const char* s, int rx, int cy, int size, uint16_t fg) {
  textAt(s, rx - textW(s, size), cy - textH(size) / 2, size, fg);
}

void numC(int n, int cx, int cy, int size, uint16_t fg) {
  char b[12];
  snprintf(b, sizeof b, "%d", n);
  textC(b, cx, cy, size, fg);
}

// ------------------------------------------------------------- The pockets ---
#define ZERO_A 0
#define ZERO_B 38

// Physical pocket order on an American wheel, used by the spin animation so
// the numbers blur past in the order they really sit.
const uint8_t WHEEL[38] = {
   0, 28,  9, 26, 30, 11,  7, 20, 32, 17,
   5, 22, 34, 15,  3, 24, 36, 13,  1, 38,
  27, 10, 25, 29, 12,  8, 19, 31, 18,  6,
  21, 33, 16,  4, 23, 35, 14,  2
};

bool isGreen(uint8_t n) { return n == ZERO_A || n == ZERO_B; }

// Picks this spin's struck numbers and their multipliers. Any pocket can be
// hit, greens included. Called once per spin, before the wheel is drawn.
void rollLightning() {
  lightCount = 0;
  if (!lightningOn) return;

  int k = 1 + (int)(esp_random() % MAX_STRIKES);     // 1..5 numbers
  for (int i = 0; i < k; i++) {
    uint8_t n;
    bool dup;
    int guard = 0;
    do {                                             // reject duplicates
      n = WHEEL[esp_random() % 38];
      dup = false;
      for (int j = 0; j < lightCount; j++) if (lightNum[j] == n) dup = true;
    } while (dup && ++guard < 50);
    if (dup) continue;

    int roll = (int)(esp_random() % LIGHT_WEIGHT_SUM);
    int m = LIGHT_MULT[N_LIGHT_MULT - 1];
    for (int j = 0; j < N_LIGHT_MULT; j++) {
      if (roll < LIGHT_WEIGHT[j]) { m = LIGHT_MULT[j]; break; }
      roll -= LIGHT_WEIGHT[j];
    }
    lightNum[lightCount]  = n;
    lightMult[lightCount] = m;
    lightCount++;
  }
}

bool isRed(uint8_t n) {
  switch (n) {
    case  1: case  3: case  5: case  7: case  9: case 12:
    case 14: case 16: case 18: case 19: case 21: case 23:
    case 25: case 27: case 30: case 32: case 34: case 36:
      return true;
  }
  return false;
}

// 0 is green like a normal wheel; 38, our second zero, gets a gold cell so
// it reads as its own thing rather than a twin of the 0 at the far end.
uint16_t pocketColor(uint8_t n) {
  if (n == ZERO_B) return C_GOLD;
  if (n == ZERO_A) return C_GREEN;
  return isRed(n) ? C_RED : C_BLACK;
}

// Black numerals on the gold cell; white everywhere else.
uint16_t pocketTextColor(uint8_t n) {
  return (n == ZERO_B) ? C_BLACK : C_WHITE;
}

// ------------------------------------------------------------------- Bets ---
int betNum[39];          // indexed by the number itself; slot 38 is zero B
int betDozen[3];         // 1-12, 13-24, 25-36
int betOutside[6];

#define OB_LOW   0
#define OB_EVEN  1
#define OB_RED   2
#define OB_BLACK 3
#define OB_ODD   4
#define OB_HIGH  5

const char* OB_LABEL[6] = { "1-18", "EVEN", "RED", "BLACK", "ODD", "19-36" };

int bankroll = 1000;

const int CHIPS[] = { 1, 5, 25, 100, 250, 1000, 2500, 5000, 10000 };
// Short labels so the big denominations still fit the chip readout at size 2.
const char* CHIP_LABEL[] = { "$1", "$5", "$25", "$100", "$250",
                             "$1K", "$2.5K", "$5K", "$10K" };
#define N_CHIPS 9
int chipIdx = 1;

// The last bet placed, saved just before the table is cleared on a spin, so
// REBET can put it all back. Stored separately from the live bet arrays.
int  lastNum[39];
int  lastDozen[3];
int  lastOutside[6];
bool hasLastBet = false;

int totalStaked() {
  int t = 0;
  for (int i = 0; i < 39; i++) t += betNum[i];
  for (int i = 0; i < 3;  i++) t += betDozen[i];
  for (int i = 0; i < 6;  i++) t += betOutside[i];
  return t;
}

// The largest chip you can actually afford right now. Money already on the
// table counts, so picking up a big bet doesn't suddenly demote your chip.
// Prevents the dead state where the selected chip exceeds your whole wallet
// and every tap silently does nothing.
int maxChipIdx() {
  int spendable = bankroll + totalStaked();
  int hi = 0;
  for (int i = 0; i < N_CHIPS; i++) if (CHIPS[i] <= spendable) hi = i;
  return hi;
}

// What the saved bet would cost to put back on the table.
int lastBetTotal() {
  int t = 0;
  for (int i = 0; i < 39; i++) t += lastNum[i];
  for (int i = 0; i < 3;  i++) t += lastDozen[i];
  for (int i = 0; i < 6;  i++) t += lastOutside[i];
  return t;
}

void saveLastBet() {
  for (int i = 0; i < 39; i++) lastNum[i]     = betNum[i];
  for (int i = 0; i < 3;  i++) lastDozen[i]   = betDozen[i];
  for (int i = 0; i < 6;  i++) lastOutside[i] = betOutside[i];
  hasLastBet = lastBetTotal() > 0;
}

void clearBets() {
  for (int i = 0; i < 39; i++) betNum[i] = 0;
  for (int i = 0; i < 3;  i++) betDozen[i] = 0;
  for (int i = 0; i < 6;  i++) betOutside[i] = 0;
}

// ------------------------------------------------------------ Persistence ---
// Accounts, settings, and the house total live in the ESP32's flash (NVS),
// so they survive being unplugged. The simulator starts with blank flash
// every run, so accounts only really persist on the real board.

#define NAME_MAX  10
#define MAX_USERS 12     // flash-space budget; delete now exists to reclaim slots

char userName[MAX_USERS][NAME_MAX + 1];
int  userPin[MAX_USERS];         // 0000-9999, stored plainly -- a fun gate, not security
int  userWallet[MAX_USERS];      // current spendable balance, carries across sessions
long userCashedOut[MAX_USERS];   // sum of every voluntary cash-out, ever
long userFromGabe[MAX_USERS];    // sum of every free handout: signup + every refill
long userBestCashOut[MAX_USERS]; // single largest cash-out ever, sets the refill tier
int  userCount = 0;

int  currentUser = -1;         // -1 = not logged in

GState state = ST_LOGIN_NAME;

// Login-flow buffers. nameBuf doubles as the username field; the on-screen
// keyboard used for it is the same one, unchanged, that used to collect a
// name at cash-out time.
char nameBuf[NAME_MAX + 1];
int  nameLen = 0;
#define PIN_LEN 4
char pinBuf[PIN_LEN + 1];
int  pinLen = 0;

// Which leaderboard tab is showing: 0 = all-time, 1 = biggest cash-out.
int lbTab = 0;

// Whose machine this is, shown under the leaderboard.
#define HOUSE_NAME "Gabe"

// Everything ever wagered minus everything ever paid out, across every
// player and every session. Net, so a lucky run can drag it back down.
long houseTake = 0;
long houseSaved = 0;

// Formats a signed amount with thousands separators: -$1,234,567
void fmtMoney(char* buf, size_t n, long v) {
  bool neg = v < 0;
  unsigned long a = neg ? (unsigned long)(-v) : (unsigned long)v;
  char tmp[24];
  int t = 0, digits = 0;
  if (a == 0) tmp[t++] = '0';
  while (a > 0) {
    tmp[t++] = (char)('0' + (a % 10));
    a /= 10;
    if (++digits % 3 == 0 && a > 0) tmp[t++] = ',';
  }
  size_t i = 0;
  if (neg && i + 1 < n) buf[i++] = '-';
  if (i + 1 < n) buf[i++] = '$';
  while (t > 0 && i + 1 < n) buf[i++] = tmp[--t];
  buf[i] = 0;
}

// Written after every settled spin; NVS appends rather than erasing in
// place, so per-spin writes land in the tens of millions before wear
// matters -- far more spins than this board will ever play. Skipped when
// the total hasn't moved (e.g. a push).
void saveHouse() {
  if (houseTake == houseSaved) return;
  prefs.putLong("house", houseTake);
  houseSaved = houseTake;
}

// The refill size for a given user, based on the biggest single cash-out
// they've ever made -- not their current balance, so going broke never
// demotes you. Applies to both bust-refills and the fresh stake handed
// out after a voluntary cash-out.
long refillAmountFor(int u) {
  if (u < 0) return 1000;
  if (userBestCashOut[u] >= 1000000) return 25000;
  if (userBestCashOut[u] >= 100000)  return 5000;
  return 1000;
}

// (everything ever cashed out + what's in the wallet right now) minus
// (everything Gabe has ever handed out for free). This is the honest
// lifetime figure: it only moves when a real bet resolves. A free refill
// adds to the wallet AND to the handout total in the same instant, so it
// cancels out and can never be mistaken for a win.
long allTimeNet(int u) {
  return (userCashedOut[u] + userWallet[u]) - userFromGabe[u];
}

void loadUsers() {
  userCount = prefs.getInt("ucount", 0);
  char k[8];
  for (int i = 0; i < userCount; i++) {
    snprintf(k, sizeof k, "u%dn", i);
    String nm = prefs.getString(k, "");
    strncpy(userName[i], nm.c_str(), NAME_MAX);
    userName[i][NAME_MAX] = 0;
    snprintf(k, sizeof k, "u%dp", i);
    userPin[i] = prefs.getInt(k, 0);
    snprintf(k, sizeof k, "u%dw", i);
    userWallet[i] = prefs.getInt(k, 1000);
    snprintf(k, sizeof k, "u%dc", i);
    userCashedOut[i] = prefs.getLong(k, 0);
    snprintf(k, sizeof k, "u%dh", i);
    userFromGabe[i] = prefs.getLong(k, 0);
    snprintf(k, sizeof k, "u%db", i);
    userBestCashOut[i] = prefs.getLong(k, 0);
  }
}

void saveUser(int i) {
  char k[8];
  snprintf(k, sizeof k, "u%dn", i);
  prefs.putString(k, userName[i]);
  snprintf(k, sizeof k, "u%dp", i);
  prefs.putInt(k, userPin[i]);
  snprintf(k, sizeof k, "u%dw", i);
  prefs.putInt(k, userWallet[i]);
  snprintf(k, sizeof k, "u%dc", i);
  prefs.putLong(k, userCashedOut[i]);
  snprintf(k, sizeof k, "u%dh", i);
  prefs.putLong(k, userFromGabe[i]);
  snprintf(k, sizeof k, "u%db", i);
  prefs.putLong(k, userBestCashOut[i]);
}

void removeUserKeys(int i) {
  char k[8];
  snprintf(k, sizeof k, "u%dn", i); prefs.remove(k);
  snprintf(k, sizeof k, "u%dp", i); prefs.remove(k);
  snprintf(k, sizeof k, "u%dw", i); prefs.remove(k);
  snprintf(k, sizeof k, "u%dc", i); prefs.remove(k);
  snprintf(k, sizeof k, "u%dh", i); prefs.remove(k);
  snprintf(k, sizeof k, "u%db", i); prefs.remove(k);
}

// Case-sensitive on purpose: the keyboard only ever produces uppercase, so
// every stored name is already uppercase and a straight compare is enough.
int findUser(const char* nm) {
  for (int i = 0; i < userCount; i++) if (!strcmp(userName[i], nm)) return i;
  return -1;
}

// Returns the new slot, or -1 if the board is full. The starting $1000
// counts as a handout from Gabe, same as any other refill, so a brand new
// account reads as exactly $0 all-time -- not a false head start.
int createUser(const char* nm, int pin) {
  if (userCount >= MAX_USERS) return -1;
  int i = userCount++;
  strncpy(userName[i], nm, NAME_MAX);
  userName[i][NAME_MAX] = 0;
  userPin[i]         = pin;
  userWallet[i]       = 1000;
  userCashedOut[i]    = 0;
  userFromGabe[i]     = 1000;
  userBestCashOut[i]  = 0;
  prefs.putInt("ucount", userCount);
  saveUser(i);
  return i;
}

// Deletes the account at slot i by shifting every later account down one
// slot (both in memory and in flash) and shrinking the count. Self-service
// only -- callers are expected to only ever pass currentUser.
void deleteUser(int i) {
  for (int j = i; j < userCount - 1; j++) {
    strcpy(userName[j], userName[j + 1]);
    userPin[j]        = userPin[j + 1];
    userWallet[j]      = userWallet[j + 1];
    userCashedOut[j]   = userCashedOut[j + 1];
    userFromGabe[j]    = userFromGabe[j + 1];
    userBestCashOut[j] = userBestCashOut[j + 1];
    saveUser(j);
  }
  removeUserKeys(userCount - 1);
  userCount--;
  prefs.putInt("ucount", userCount);
}

// ---------------------------------------------------------------- History ---
// ---------------------------------------------------------------- History ---
#define MAX_HIST 15
uint8_t hist[MAX_HIST];
int histCount = 0;

void pushHistory(uint8_t n) {
  if (histCount == MAX_HIST) {
    for (int i = 0; i < MAX_HIST - 1; i++) hist[i] = hist[i + 1];
    hist[MAX_HIST - 1] = n;
  } else {
    hist[histCount++] = n;
  }
}

// ----------------------------------------------------------------- Layout ---
#define HDR_H     22
#define HIST_Y    22
#define HIST_H    18

#define GRID_Y    42
#define ROW_H     26
#define GRID_H    (ROW_H * 3)

// 28 + 264 + 28 = 320 exactly, and 264 divides evenly by 12, 6 and 3,
// so no dead pixels fall between cells.
#define ZERO_W    28
#define GRID_X    ZERO_W
#define GRID_W    264
#define COL_W     (GRID_W / 12)      // 22 px per number
#define ZB_X      (GRID_X + GRID_W)  // 292

#define DOZ_Y     122
#define DOZ_H     24
#define EVEN_Y    148
#define EVEN_H    24

#define PANEL_Y   176

const Zone Z_MINUS = {   2, 182, 24, 26 };
const Zone Z_PLUS  = {  98, 182, 24, 26 };
const Zone Z_CLEAR = { 128, 182, 80, 26 };
const Zone Z_SPIN  = { 216, 182, 100, 50 };

// Second panel row, tucked to the left of the tall SPIN button.
const Zone Z_REBET = {   2, 210, 100, 26 };
const Zone Z_2X    = { 108, 210, 100, 26 };

// The chip readout sits between the - and + buttons. Sized so the widest
// label ($2.5K) clears the edges instead of touching them.
#define CHIPBOX_X 28
#define CHIPBOX_W 68

// Tapping your bankroll in the header opens the leaderboard.
const Zone Z_BANK  = { 220,   0, 100, HDR_H };

// Leaderboard: two tabs up top, MY ACCOUNT + BACK at the bottom.
const Zone Z_TAB1    = {  20,  24, 138, 20 };   // ALL-TIME
const Zone Z_TAB2    = { 162,  24, 138, 20 };   // BIG WINS
const Zone Z_MYACCT  = {  16, 206, 140, 26 };
const Zone Z_LBBACK  = { 180, 206, 124, 26 };

// Account/settings screen.
const Zone Z_CASHOUT = {  16,  86, 288, 34 };
const Zone Z_VOL     = {  16, 128, 138, 30 };
const Zone Z_DELETE  = { 162, 128, 142, 30 };
const Zone Z_WHEEL   = {  16, 164, 138, 24 };
const Zone Z_LIGHT   = { 162, 164, 142, 24 };
const Zone Z_LOGOUT  = {  16, 192, 138, 24 };
const Zone Z_ACCBACK = { 162, 192, 142, 24 };

// Delete confirmation.
const Zone Z_DELYES  = {  30, 150, 120, 40 };
const Zone Z_DELNO   = { 170, 150, 120, 40 };

// On-screen A-Z0-9 keyboard: 10 columns x 4 rows of 32x40 cells. Used for
// username entry.
#define KB_X0   0
#define KB_Y0   76
#define KB_CW   32
#define KB_CH   40
#define KB_COLS 10
#define KB_ROWS 4
#define KB_NCHARS 36
#define KB_SPC  36
#define KB_DEL  37
#define KB_OK   38
const char* KB_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// Numeric PIN pad: a 3x4 phone-style grid, cells 80x36.
// 1 2 3 / 4 5 6 / 7 8 9 / DEL 0 OK
#define PIN_X0  40
#define PIN_Y0  86
#define PIN_CW  80
#define PIN_CH  36
const char* PIN_LAYOUT[12] = {
  "1","2","3", "4","5","6", "7","8","9", "DEL","0","OK"
};

bool inZone(const Zone &z, int x, int y) {
  return x >= z.x && x < z.x + z.w && y >= z.y && y < z.y + z.h;
}

// Column 0 holds 1/2/3, column 1 holds 4/5/6, and so on.
// Row 0 is the top (3, 6, 9...), row 2 the bottom (1, 4, 7...).
void numberCell(uint8_t n, int &x, int &y, int &w, int &h) {
  if (n == ZERO_A) { x = 0;    y = GRID_Y; w = ZERO_W;       h = GRID_H; return; }
  if (n == ZERO_B) { x = ZB_X; y = GRID_Y; w = SCR_W - ZB_X; h = GRID_H; return; }
  int col = (n - 1) / 3;
  int rem = (n - 1) % 3;
  int row = 2 - rem;
  x = GRID_X + col * COL_W;
  y = GRID_Y + row * ROW_H;
  w = COL_W;
  h = ROW_H;
}

int numberAt(int x, int y) {
  if (y < GRID_Y || y >= GRID_Y + GRID_H) return -1;
  if (x < GRID_X)  return ZERO_A;
  if (x >= ZB_X)   return ZERO_B;
  int col = (x - GRID_X) / COL_W;
  if (col > 11) return -1;
  int row = (y - GRID_Y) / ROW_H;
  if (row > 2) row = 2;
  int rem = 2 - row;
  return col * 3 + rem + 1;
}

// ---------------------------------------------------------------- Drawing ---
void drawHeader() {
  tft.fillRect(0, 0, SCR_W, HDR_H, C_PANEL);
  textAt("ROULETTE", 4, HDR_H / 2 - 8, 2, C_GOLD);

  char buf[16];
  snprintf(buf, sizeof buf, "$%d", bankroll);
  textR(buf, SCR_W - 4, HDR_H / 2, 2, C_WHITE);
}

void drawHistory() {
  tft.fillRect(0, HIST_Y, SCR_W, HIST_H, C_BLACK);
  int cw = 21;
  for (int i = 0; i < histCount; i++) {
    uint8_t n = hist[i];
    int x = 4 + i * cw;
    uint16_t c = pocketColor(n);
    tft.fillRoundRect(x, HIST_Y + 2, cw - 3, HIST_H - 4, 2, c);
    tft.drawRoundRect(x, HIST_Y + 2, cw - 3, HIST_H - 4, 2, C_LINE);
    numC(n, x + (cw - 3) / 2, HIST_Y + HIST_H / 2, 1, pocketTextColor(n));
  }
}

// A chip showing the amount riding on a wide cell. It grows to fit the
// number, so a $1000 stack doesn't spill outside the disc.
void drawChipMarker(int cx, int cy, int amount) {
  if (amount <= 0) return;
  char b[12];
  snprintf(b, sizeof b, "%d", amount);
  int w = textW(b, 1) + 9;
  if (w < 18) w = 18;
  int h = 15;
  tft.fillRoundRect(cx - w / 2, cy - h / 2, w, h, 7, C_GOLD);
  tft.drawRoundRect(cx - w / 2, cy - h / 2, w, h, 7, C_BLACK);
  textC(b, cx, cy, 1, C_BLACK);
}

// Same chip, but anchored so its right edge lands on rx.
void drawChipMarkerRight(int rx, int cy, int amount) {
  if (amount <= 0) return;
  char b[12];
  snprintf(b, sizeof b, "%d", amount);
  int w = textW(b, 1) + 9;
  if (w < 18) w = 18;
  drawChipMarker(rx - w / 2, cy, amount);
}

// Number cells are only 22px wide, so a full chip would bury the number.
// A gold outline plus a corner pip marks the bet and keeps the digits legible.
void drawNumberBetMark(int x, int y, int w, int h, int amount) {
  if (amount <= 0) return;
  tft.drawRect(x + 1, y + 1, w - 2, h - 2, C_GOLD);
  tft.fillCircle(x + 5, y + 5, 3, C_GOLD);
}

void drawOneNumber(uint8_t n) {
  int x, y, w, h;
  numberCell(n, x, y, w, h);
  uint16_t c = pocketColor(n);
  tft.fillRect(x, y, w, h, c);
  tft.drawRect(x, y, w, h, C_LINE);
  numC(n, x + w / 2, y + h / 2, isGreen(n) ? 2 : 1, pocketTextColor(n));
  drawNumberBetMark(x, y, w, h, betNum[n]);
}

void drawGrid() {
  drawOneNumber(ZERO_A);
  drawOneNumber(ZERO_B);
  for (uint8_t n = 1; n <= 36; n++) drawOneNumber(n);
}

void drawDozens() {
  int w = GRID_W / 3;
  const char* lbl[3] = { "1-12", "13-24", "25-36" };
  for (int i = 0; i < 3; i++) {
    int x = GRID_X + i * w;
    tft.fillRect(x, DOZ_Y, w, DOZ_H, C_FELT);
    tft.drawRect(x, DOZ_Y, w, DOZ_H, C_LINE);
    textC(lbl[i], x + w / 2, DOZ_Y + DOZ_H / 2, 2, C_WHITE);
    drawChipMarkerRight(x + w - 3, DOZ_Y + DOZ_H / 2, betDozen[i]);
  }
}

void drawEvenMoney() {
  int w = GRID_W / 6;
  for (int i = 0; i < 6; i++) {
    int x = GRID_X + i * w;
    uint16_t bg = C_FELT;
    if (i == OB_RED)   bg = C_RED;
    if (i == OB_BLACK) bg = C_BLACK;
    tft.fillRect(x, EVEN_Y, w, EVEN_H, bg);
    tft.drawRect(x, EVEN_Y, w, EVEN_H, C_LINE);
    textC(OB_LABEL[i], x + w / 2, EVEN_Y + 7, 1, C_WHITE);
    drawChipMarker(x + w / 2, EVEN_Y + EVEN_H - 8, betOutside[i]);
  }
}

void drawButton(const Zone &z, const char* label, uint16_t col, int size) {
  tft.fillRoundRect(z.x, z.y, z.w, z.h, 4, C_PANEL);
  tft.drawRoundRect(z.x, z.y, z.w, z.h, 4, col);
  textC(label, z.x + z.w / 2, z.y + z.h / 2, size, col);
}

void drawChipValue() {
  tft.fillRect(CHIPBOX_X, 182, CHIPBOX_W, 26, C_BLACK);
  textC(CHIP_LABEL[chipIdx], CHIPBOX_X + CHIPBOX_W / 2, 195, 2, C_GOLD);
}

// REBET and 2X share a row and change with table state, so they get their
// own redraw rather than forcing a whole-panel repaint on every chip tap.
// REBET greys out with no saved bet; 2X only appears once money is down.
void drawActionButtons() {
  tft.fillRect(0, 210, 212, 28, C_BLACK);

  bool canRebet = hasLastBet && bankroll >= lastBetTotal();
  drawButton(Z_REBET, "REBET", canRebet ? C_GOLD : C_GREY, 2);

  int staked = totalStaked();
  if (staked > 0) {
    drawButton(Z_2X, "2X", bankroll >= staked ? C_WIN : C_GREY, 2);
  }
}

void drawPanel() {
  tft.fillRect(0, PANEL_Y, SCR_W, SCR_H - PANEL_Y, C_BLACK);
  drawButton(Z_MINUS, "-", C_WHITE, 2);
  drawButton(Z_PLUS,  "+", C_WHITE, 2);
  drawChipValue();
  drawButton(Z_CLEAR, "CLEAR", C_WHITE, 2);
  drawButton(Z_SPIN,  "SPIN",  C_GOLD, 3);
  drawActionButtons();
}

void drawTable() {
  tft.fillRect(0, GRID_Y - 2, SCR_W, PANEL_Y - GRID_Y + 2, C_BLACK);
  drawGrid();
  drawDozens();
  drawEvenMoney();
}

void drawAll() {
  tft.fillScreen(C_BLACK);
  drawHeader();
  drawHistory();
  drawTable();
  drawPanel();
}

// ------------------------------------------------------------ Score screen ---
// Defined much further down (in the keyboard-cursor section), but logInAs
// below needs to call it to show/hide the WASD cursor on login.
void drawCursorBox();
// Defined further down (Login: name section); doDeleteCurrentUser needs it.
void drawLoginName();

// ------------------------------------------------------------ Leaderboard ---
// Auto-computed from stored accounts, sorted by lifetime net profit -- no
// manual name entry, since your identity comes from login now.
void drawScoresScreen() {
  tft.fillScreen(C_BLACK);
  tft.fillRect(0, 0, SCR_W, HDR_H, C_PANEL);
  textC(lbTab == 0 ? "ALL-TIME" : "TOP PLAYERS", 160, HDR_H / 2, 2, C_GOLD);

  // Two tabs: which one is active gets a gold outline.
  drawButton(Z_TAB1, "ALL-TIME", lbTab == 0 ? C_GOLD : C_GREY, 1);
  drawButton(Z_TAB2, "BIG WINS", lbTab == 1 ? C_GOLD : C_GREY, 1);

  // Simple selection sort over a small array; userCount maxes out at 12.
  int order[MAX_USERS];
  for (int i = 0; i < userCount; i++) order[i] = i;
  for (int i = 0; i < userCount; i++) {
    int best = i;
    for (int j = i + 1; j < userCount; j++) {
      long a = lbTab == 0 ? allTimeNet(order[j])    : userBestCashOut[order[j]];
      long b = lbTab == 0 ? allTimeNet(order[best]) : userBestCashOut[order[best]];
      if (a > b) best = j;
    }
    int t = order[i]; order[i] = order[best]; order[best] = t;
  }

  // Names are truncated to 6 characters for display only (the full name is
  // still what's used to log in) so a long name can never collide with a
  // wide negative amount like -$987,654,321 in the same row.
  #define NAME_SHOW 6

  int rows = userCount < 5 ? userCount : 5;
  for (int r = 0; r < rows; r++) {
    int u = order[r];
    int y = 50 + r * 22;
    uint16_t col = (u == currentUser) ? C_GOLD : C_WHITE;

    char rank[4];
    snprintf(rank, sizeof rank, "%d.", r + 1);
    textAt(rank, 10, y, 2, col);

    char shown[NAME_SHOW + 1];
    strncpy(shown, userName[u], NAME_SHOW);
    shown[NAME_SHOW] = 0;
    textAt(shown, 42, y, 2, col);

    long val = lbTab == 0 ? allTimeNet(u) : userBestCashOut[u];
    char v[24];
    fmtMoney(v, sizeof v, val);
    textR(v, SCR_W - 10, y + 8, 2, val < 0 ? C_LOSE : col);
  }

  // The house's running total, small, under a divider.
  tft.drawFastHLine(10, 172, SCR_W - 20, C_LINE);
  char money[28], line[48];
  fmtMoney(money, sizeof money, houseTake);
  snprintf(line, sizeof line, "%s's $$$: %s", HOUSE_NAME, money);
  textC(line, 160, 184, 1, houseTake < 0 ? C_LOSE : C_GOLD);

  drawButton(Z_MYACCT, "MY ACCOUNT", C_GOLD, 1);
  drawButton(Z_LBBACK, "BACK", C_WHITE, 1);
}

// -------------------------------------------------------------- Account ---
void drawAccountScreen() {
  tft.fillScreen(C_BLACK);
  tft.fillRect(0, 0, SCR_W, HDR_H, C_PANEL);
  textC(userName[currentUser], 160, HDR_H / 2, 2, C_GOLD);

  char wal[24];
  fmtMoney(wal, sizeof wal, bankroll);
  char walLine[40];
  snprintf(walLine, sizeof walLine, "Wallet: %s", wal);
  textC(walLine, 160, 38, 2, C_WHITE);

  char cashLbl[48];
  char cashAmt[24];
  fmtMoney(cashAmt, sizeof cashAmt, bankroll);
  snprintf(cashLbl, sizeof cashLbl, "CASH OUT %s", cashAmt);
  drawButton(Z_CASHOUT, cashLbl, bankroll > 0 ? C_GOLD : C_GREY, 2);

  char tier[56];
  fmtMoney(wal, sizeof wal, refillAmountFor(currentUser));
  snprintf(tier, sizeof tier, "Next refill from Gabe: %s", wal);
  textC(tier, 160, 75, 1, C_GREY);

  char vol[16];
  snprintf(vol, sizeof vol, "Vol: %s", VOL_LABEL[volLevel]);
  drawButton(Z_VOL, vol, C_WHITE, 1);
  drawButton(Z_DELETE, "DELETE ACCT", C_LOSE, 1);

  drawButton(Z_WHEEL, useWheel ? "VIEW: WHEEL" : "VIEW: STRIP", C_WHITE, 1);
  drawButton(Z_LIGHT, lightningOn ? "LIGHTNING: ON" : "LIGHTNING: OFF",
             lightningOn ? C_GOLD : C_GREY, 1);

  drawButton(Z_LOGOUT, "LOG OUT", C_GOLD, 1);
  drawButton(Z_ACCBACK, "BACK", C_WHITE, 1);
}

void drawConfirmDelete() {
  tft.fillScreen(C_BLACK);
  tft.fillRect(0, 0, SCR_W, HDR_H, C_PANEL);
  textC("DELETE ACCOUNT?", 160, HDR_H / 2, 2, C_LOSE);

  char msg[48];
  snprintf(msg, sizeof msg, "Erase %s and all their history?", userName[currentUser]);
  textC(msg, 160, 70, 1, C_WHITE);
  textC("This cannot be undone.", 160, 90, 1, C_GREY);

  drawButton(Z_DELYES, "YES, DELETE", C_LOSE, 1);
  drawButton(Z_DELNO,  "CANCEL",      C_WHITE, 1);
}

void doDeleteCurrentUser() {
  deleteUser(currentUser);
  currentUser = -1;
  nameLen = 0; nameBuf[0] = 0;
  sLose();
  state = ST_LOGIN_NAME;
  drawLoginName();
}

// -------------------------------------------------------------- Login: name ---
void drawNameText() {
  tft.fillRect(0, 30, SCR_W, 40, C_BLACK);
  const char* shown = nameLen ? nameBuf : "_";
  textC(shown, 160, 48, 3, C_GOLD);
}

void drawLoginName() {
  tft.fillScreen(C_BLACK);
  tft.fillRect(0, 0, SCR_W, HDR_H, C_PANEL);
  textC("TINY BAR CASINO", 160, HDR_H / 2, 2, C_GOLD);

  drawNameText();
  textC("Enter your name -- new or returning", 160, 70, 1, C_GREY);

  for (int r = 0; r < KB_ROWS; r++) {
    for (int c = 0; c < KB_COLS; c++) {
      int idx = r * KB_COLS + c;
      int x = KB_X0 + c * KB_CW, y = KB_Y0 + r * KB_CH;
      tft.drawRect(x, y, KB_CW, KB_CH, C_LINE);

      char lbl[4];
      int size = 2;
      uint16_t col = C_WHITE;
      if (idx < KB_NCHARS) {
        lbl[0] = KB_CHARS[idx]; lbl[1] = 0;
      } else if (idx == KB_SPC) {
        strcpy(lbl, "SP"); col = C_GREY;
      } else if (idx == KB_DEL) {
        strcpy(lbl, "DEL"); size = 1; col = C_GREY;
      } else {
        strcpy(lbl, "OK"); col = C_GOLD;
      }
      textC(lbl, x + KB_CW / 2, y + KB_CH / 2, size, col);
    }
  }
}

void typeChar(char ch) {
  if (nameLen >= NAME_MAX) return;
  nameBuf[nameLen++] = ch;
  nameBuf[nameLen] = 0;
  sClick();
  drawNameText();
}

void backspace() {
  if (nameLen == 0) return;
  nameBuf[--nameLen] = 0;
  sClick();
  drawNameText();
}

// ------------------------------------------------------------- Login: PIN ---
void drawPinDots() {
  tft.fillRect(0, 30, SCR_W, 40, C_BLACK);
  char dots[PIN_LEN + 1];
  int i = 0;
  for (; i < pinLen; i++) dots[i] = '*';
  dots[i] = 0;
  const char* shown = pinLen ? dots : "----";
  textC(shown, 160, 48, 4, C_GOLD);
}

void drawPinPad(const char* title, const char* sub) {
  tft.fillScreen(C_BLACK);
  tft.fillRect(0, 0, SCR_W, HDR_H, C_PANEL);
  textC(title, 160, HDR_H / 2, 2, C_GOLD);

  drawPinDots();
  textC(sub, 160, 74, 1, C_GREY);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      int idx = r * 3 + c;
      int x = PIN_X0 + c * PIN_CW, y = PIN_Y0 + r * PIN_CH;
      tft.drawRect(x, y, PIN_CW, PIN_CH, C_LINE);
      const char* lbl = PIN_LAYOUT[idx];
      uint16_t col = C_WHITE;
      if (!strcmp(lbl, "OK"))  col = C_GOLD;
      if (!strcmp(lbl, "DEL")) col = C_GREY;
      textC(lbl, x + PIN_CW / 2, y + PIN_CH / 2, 2, col);
    }
  }
}

void drawLoginPin() {
  char sub[NAME_MAX + 32];
  snprintf(sub, sizeof sub, "Welcome back, %s. Enter your PIN.", nameBuf);
  drawPinPad("ENTER PIN", sub);
}

void drawNewPin() {
  drawPinPad("CHOOSE A PIN", "New account -- pick any 4 digits");
}

void pinDigit(char d) {
  if (pinLen >= PIN_LEN) return;
  pinBuf[pinLen++] = d;
  pinBuf[pinLen] = 0;
  sClick();
  drawPinDots();
}

void pinBackspace() {
  if (pinLen == 0) return;
  pinBuf[--pinLen] = 0;
  sClick();
  drawPinDots();
}

// Shown whenever Gabe hands over free money: after busting, or when logging
// into an account that's already at $0 (this is the fix for the bug where a
// broke account could never spin, and so could never trigger a refill --
// the old check only ran after a spin settled, which a $0 wallet can never
// do). Also used, with different framing, right after a cash-out.
void showRefillScreen(long amt, bool isBust) {
  tft.fillScreen(C_BLACK);
  if (isBust) {
    ledSet(true, false, false);
    textC("BUSTED", 160, 76, 4, C_LOSE);
    sBust();
  } else {
    ledSet(false, true, false);
    textC("Cashed out!", 160, 76, 3, C_WIN);
    sWin();
  }
  char line[32];
  fmtMoney(line, sizeof line, amt);
  char msg[64];
  snprintf(msg, sizeof msg, "Tap or send x for %s from Gabe", line);
  textC(msg, 160, 130, 1, C_WHITE);
  waitForTap();
  ledOff();
}

// Grants a tiered refill: sets the wallet, counts it as a handout (so the
// all-time figure can't be inflated by it), and saves immediately.
void giveRefill(int u, bool isBust) {
  long amt = refillAmountFor(u);
  userWallet[u]  = amt;
  userFromGabe[u] += amt;
  bankroll = amt;
  // A $10K chip left over from a big session would be unusable on a $1K
  // refill, and every tap would silently do nothing. Clamp it down.
  if (chipIdx > maxChipIdx()) chipIdx = maxChipIdx();
  saveUser(u);
  showRefillScreen(amt, isBust);
}

// Loads an account into the live game state and goes to the table. If the
// wallet is already broke, this is the one place that can catch it -- a
// broke wallet can never place a bet, so it can never reach the post-spin
// bust check on its own.
void logInAs(int u) {
  currentUser = u;
  bankroll = userWallet[u];
  clearBets();
  histCount = 0;
  sWin();

  if (bankroll < CHIPS[0]) giveRefill(u, true);

  state = ST_BET;
  drawAll();
  drawCursorBox();
}

// Banks the whole wallet, updates the personal-best cash-out (which may
// unlock a bigger refill tier), then immediately hands over a fresh stake
// -- "cash out while you're up, and Gabe puts $1,000 (or more) back in."
void doCashOut() {
  if (currentUser < 0 || bankroll <= 0) return;
  int u = currentUser;
  userCashedOut[u] += bankroll;
  if (bankroll > userBestCashOut[u]) userBestCashOut[u] = bankroll;
  saveUser(u);

  giveRefill(u, false);

  clearBets();
  histCount = 0;
  drawAll();
  drawCursorBox();
}

void tryLogin() {
  int u = findUser(nameBuf);
  if (u < 0) {
    // New name: go set a PIN for it.
    pinLen = 0; pinBuf[0] = 0;
    if (userCount >= MAX_USERS) {
      textC("Board is full (8/8 accounts).", 160, 70, 1, C_LOSE);
      textC("Ask Gabe to clear a slot, or log in as someone else.", 160, 82, 1, C_LOSE);
      sLose();
      return;
    }
    state = ST_NEW_PIN;
    drawNewPin();
    return;
  }
  pinLen = 0; pinBuf[0] = 0;
  state = ST_LOGIN_PIN;
  drawLoginPin();
}

void submitName() {
  while (nameLen > 0 && nameBuf[nameLen - 1] == ' ') nameBuf[--nameLen] = 0;
  if (nameLen == 0) return;
  tryLogin();
}

void submitLoginPin() {
  if (pinLen < PIN_LEN) return;
  int u = findUser(nameBuf);
  int entered = atoi(pinBuf);
  if (u >= 0 && userPin[u] == entered) {
    logInAs(u);
  } else {
    sLose();
    pinLen = 0; pinBuf[0] = 0;
    drawLoginPin();
    textC("Wrong PIN, try again", 160, 74, 1, C_LOSE);
  }
}

void submitNewPin() {
  if (pinLen < PIN_LEN) return;
  int u = createUser(nameBuf, atoi(pinBuf));
  if (u < 0) { state = ST_LOGIN_NAME; drawLoginName(); return; }
  logInAs(u);
}

// ---------------------------------------------------------------- Placing ---
// ---------------------------------------------------------------- Placing ---
bool placeBet(int x, int y) {
  int chip = CHIPS[chipIdx];
  if (bankroll < chip) return false;

  int n = numberAt(x, y);
  if (n >= 0) {
    betNum[n] += chip;
    bankroll  -= chip;
    drawOneNumber(n);
    return true;
  }

  if (y >= DOZ_Y && y < DOZ_Y + DOZ_H && x >= GRID_X && x < ZB_X) {
    int i = (x - GRID_X) / (GRID_W / 3);
    if (i > 2) i = 2;
    betDozen[i] += chip;
    bankroll    -= chip;
    drawDozens();
    return true;
  }

  if (y >= EVEN_Y && y < EVEN_Y + EVEN_H && x >= GRID_X && x < ZB_X) {
    int i = (x - GRID_X) / (GRID_W / 6);
    if (i > 5) i = 5;
    betOutside[i] += chip;
    bankroll      -= chip;
    drawEvenMoney();
    return true;
  }

  return false;
}

// ------------------------------------------------------- Rebet / double ---
// Puts the previous bet back on the table, chip for chip. Refuses rather
// than partially filling if the wallet can't cover the whole thing --
// a half-restored bet would be worse than none, since you'd think you had
// your old spread and actually have something else.
void doRebet() {
  if (!hasLastBet) return;
  int need = lastBetTotal();
  if (need <= 0) return;
  if (bankroll < need) { sLose(); return; }

  for (int i = 0; i < 39; i++) betNum[i]     = lastNum[i];
  for (int i = 0; i < 3;  i++) betDozen[i]   = lastDozen[i];
  for (int i = 0; i < 6;  i++) betOutside[i] = lastOutside[i];
  bankroll -= need;

  sChip();
  drawHeader();
  drawTable();
  drawActionButtons();
}

// Doubles every bet currently on the table. Same all-or-nothing rule: if
// you can't cover the full double, nothing moves. Stacks, so hitting it
// twice is 4x -- as long as the wallet keeps up.
void doDoubleBets() {
  int staked = totalStaked();
  if (staked <= 0) return;
  if (bankroll < staked) { sLose(); return; }

  for (int i = 0; i < 39; i++) betNum[i]     *= 2;
  for (int i = 0; i < 3;  i++) betDozen[i]   *= 2;
  for (int i = 0; i < 6;  i++) betOutside[i] *= 2;
  bankroll -= staked;

  sChip();
  drawHeader();
  drawTable();
  drawActionButtons();
}

// --------------------------------------------------------- Keyboard play ---
// The simulator has no touch, so the board can also be driven from the serial
// monitor. The cursor stays hidden until the first key arrives, so on real
// hardware -- where nothing is typed -- none of this ever shows up.
// Defined further down; the key handler and login screens need them early.
void doSpin();
void resetGame();
void openScores();
void submitName();
void tryLogin();
void submitLoginPin();
void submitNewPin();
void typeChar(char ch);
void backspace();
void pinDigit(char d);
void pinBackspace();
void cycleVolume();
void drawCursorBox();
void drawAll();

bool kbActive = false;
int curSec = 0;      // 0 = number grid, 1 = dozens, 2 = even money
int curCol = 6;      // 0 = zero A, 1..12 = number columns, 13 = zero B
int curRow = 1;      // 0 = top row, 2 = bottom row
int curDz  = 1;
int curOb  = 2;

// The number under the cursor, or -1 if the cursor isn't on the grid.
int cursorNumber() {
  if (curSec != 0) return -1;
  if (curCol == 0)  return ZERO_A;
  if (curCol == 13) return ZERO_B;
  return (curCol - 1) * 3 + (2 - curRow) + 1;
}

Zone cursorZone() {
  Zone z;
  if (curSec == 0) {
    int n = cursorNumber();
    numberCell((uint8_t)n, z.x, z.y, z.w, z.h);
  } else if (curSec == 1) {
    z.w = GRID_W / 3;
    z.x = GRID_X + curDz * z.w;
    z.y = DOZ_Y;  z.h = DOZ_H;
  } else {
    z.w = GRID_W / 6;
    z.x = GRID_X + curOb * z.w;
    z.y = EVEN_Y; z.h = EVEN_H;
  }
  return z;
}

void redrawUnderCursor() {
  if      (curSec == 0) drawOneNumber((uint8_t)cursorNumber());
  else if (curSec == 1) drawDozens();
  else                  drawEvenMoney();
}

void drawCursorBox() {
  if (!kbActive) return;
  Zone z = cursorZone();
  tft.drawRect(z.x,     z.y,     z.w,     z.h,     C_WHITE);
  tft.drawRect(z.x + 1, z.y + 1, z.w - 2, z.h - 2, C_WHITE);
}

void moveCursor(int dx, int dy) {
  redrawUnderCursor();
  if (dy < 0) {                               // up
    if      (curSec == 2) { curSec = 1; curDz = curOb / 2; }
    else if (curSec == 1) { curSec = 0; curRow = 2; }
    else if (curRow > 0)  curRow--;
  } else if (dy > 0) {                        // down
    if      (curSec == 0 && curRow < 2) curRow++;
    else if (curSec == 0) { curSec = 1; }
    else if (curSec == 1) { curSec = 2; curOb = curDz * 2; }
  }
  if (dx != 0) {
    if      (curSec == 0) curCol = constrain(curCol + dx, 0, 13);
    else if (curSec == 1) curDz  = constrain(curDz  + dx, 0, 2);
    else                  curOb  = constrain(curOb  + dx, 0, 5);
  }
  drawCursorBox();
  sClick();
}

void placeAtCursor() {
  int chip = CHIPS[chipIdx];
  if (bankroll < chip) return;
  if (curSec == 0) {
    int n = cursorNumber();
    betNum[n] += chip; bankroll -= chip;
    drawOneNumber((uint8_t)n);
  } else if (curSec == 1) {
    betDozen[curDz] += chip; bankroll -= chip;
    drawDozens();
  } else {
    betOutside[curOb] += chip; bankroll -= chip;
    drawEvenMoney();
  }
  drawCursorBox();
  drawHeader();
  drawActionButtons();
  sChip();
}

void printKeyHelp() {
  Serial.println("Keyboard: w/a/s/d move  p place  g spin  c clear  - + chip");
  Serial.println("          k scores/cash out  m mute");
  Serial.println("(type into the box under the serial monitor, then press Enter)");
}

void handleKey(char k) {
  // Login/PIN screens don't use the WASD cursor at all -- handle them first,
  // completely separately, before any of that machinery kicks in.
  if (state == ST_LOGIN_NAME) {
    if (k == '\n' || k == '\r') { submitName(); return; }
    if (k == 8 || k == 127 || k == '<') { backspace(); return; }
    if (k >= 'a' && k <= 'z') k = k - 'a' + 'A';
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ') typeChar(k);
    return;
  }
  if (state == ST_LOGIN_PIN || state == ST_NEW_PIN) {
    if (k == '\n' || k == '\r') {
      if (state == ST_LOGIN_PIN) submitLoginPin(); else submitNewPin();
      return;
    }
    if (k == 8 || k == 127 || k == '<') { pinBackspace(); return; }
    if (k >= '0' && k <= '9') pinDigit(k);
    return;
  }

  if (!kbActive && k != '\n' && k != '\r') {
    kbActive = true; drawCursorBox(); printKeyHelp();
  }

  if (k == '\n' || k == '\r') return;    // Wokwi sends a newline per line

  if (state == ST_SCORES) {
    if (k == '1')                      { lbTab = 0; sClick(); drawScoresScreen(); }
    else if (k == '2')                 { lbTab = 1; sClick(); drawScoresScreen(); }
    else if (k == 'a' || k == 'A')     { sClick(); state = ST_ACCOUNT; drawAccountScreen(); }
    else if (k == 'b' || k == 'B' ||
             k == 'k' || k == 'K')     { sClick(); state = ST_BET; drawAll(); drawCursorBox(); }
    return;
  }

  if (state == ST_ACCOUNT) {
    if (k == 'x' || k == 'X')          { if (bankroll > 0) doCashOut(); }
    else if (k == 'v' || k == 'V')     { cycleVolume(); drawAccountScreen(); }
    else if (k == 'w' || k == 'W')     { useWheel = !useWheel; prefs.putBool("wheel", useWheel); sClick(); drawAccountScreen(); }
    else if (k == 'l' || k == 'L')     { lightningOn = !lightningOn; prefs.putBool("lit", lightningOn); sClick(); drawAccountScreen(); }
    else if (k == 'd' || k == 'D')     { sClick(); state = ST_CONFIRM_DELETE; drawConfirmDelete(); }
    else if (k == 'o' || k == 'O')     {
      sClick(); currentUser = -1; nameLen = 0; nameBuf[0] = 0;
      state = ST_LOGIN_NAME; drawLoginName();
    } else if (k == 'b' || k == 'B')   { sClick(); state = ST_SCORES; drawScoresScreen(); }
    return;
  }

  if (state == ST_CONFIRM_DELETE) {
    if (k == 'y' || k == 'Y') doDeleteCurrentUser();
    else if (k == 'n' || k == 'N') { sClick(); state = ST_ACCOUNT; drawAccountScreen(); }
    return;
  }

  switch (k) {
    case 'w': case 'W': moveCursor(0, -1); break;
    case 's': case 'S': moveCursor(0,  1); break;
    case 'a': case 'A': moveCursor(-1, 0); break;
    case 'd': case 'D': moveCursor( 1, 0); break;
    case 'p': case 'P': case ' ': placeAtCursor(); break;
    case 'g': case 'G':
      if (totalStaked() > 0) { doSpin(); drawCursorBox(); }
      break;
    case 'k': case 'K': sClick(); openScores(); break;
    case 'r': case 'R': sClick(); doRebet(); drawCursorBox(); break;
    case 't': case 'T':
      if (totalStaked() > 0) { sClick(); doDoubleBets(); drawCursorBox(); }
      break;
    case 'c': case 'C':
      bankroll += totalStaked();
      clearBets();
      drawHeader(); drawTable(); drawActionButtons(); drawCursorBox();
      sClick();
      break;
    case '-': case '_':
      if (chipIdx > 0) { chipIdx--; drawChipValue(); sClick(); }
      break;
    case '+': case '=':
      if (chipIdx < maxChipIdx()) { chipIdx++; drawChipValue(); sClick(); }
      break;
    case 'v': case 'V':
      cycleVolume();
      break;
    default: break;
  }
}

// ------------------------------------------------------------- The spin -----
// A strip of pockets rips past a marker and decelerates onto the winner.
#define STRIP_Y  (GRID_Y + 14)
#define STRIP_H  40
#define TILE_W   44
#define WHEEL_PX (38 * TILE_W)

// Draws the strip at a given scroll offset.
//
// `detail` controls whether numbers are drawn. Adafruit_GFX renders text one
// font pixel at a time -- a two-digit number is ~80 separate draw calls -- so
// numbers cost far more than the colored blocks behind them. During the fast
// blur we skip them entirely (you couldn't read them anyway, and a real wheel
// is a blur too), and even when slow we only letter the tiles near the marker.
void drawStrip(int offset, bool detail) {
  int cy = STRIP_Y + STRIP_H / 2;
  int first = offset / TILE_W;
  int shift = offset % TILE_W;

  for (int i = -1; i <= SCR_W / TILE_W + 1; i++) {
    int idx = ((first + i) % 38 + 38) % 38;
    uint8_t n = WHEEL[idx];
    int x = i * TILE_W - shift;
    int cxTile = x + (TILE_W - 2) / 2;
    if (cxTile < -TILE_W || cxTile > SCR_W + TILE_W) continue;

    uint16_t c = pocketColor(n);
    tft.fillRect(x, STRIP_Y, TILE_W - 2, STRIP_H, c);
    tft.drawRect(x, STRIP_Y, TILE_W - 2, STRIP_H, C_LINE);

    // Only letter the three tiles closest to the marker.
    if (detail && cxTile > 160 - 90 && cxTile < 160 + 90) {
      numC(n, cxTile, cy, 3, pocketTextColor(n));
    }
  }

  tft.fillTriangle(160, STRIP_Y - 9, 153, STRIP_Y - 1, 167, STRIP_Y - 1, C_GOLD);
  tft.drawFastVLine(160, STRIP_Y, STRIP_H, C_GOLD);
}

// How long a spin lasts, in milliseconds. This is wall-clock time, not a
// frame count, so it holds regardless of how fast the board can draw.
// ------------------------------------------------------- Lightning reveal ---
void drawLightningReveal() {
  if (!lightningOn || lightCount == 0) return;

  tft.fillRect(0, GRID_Y - 2, SCR_W, PANEL_Y - GRID_Y + 2, C_BLACK);
  textC("** LIGHTNING **", 160, GRID_Y + 12, 2, C_GOLD);

  for (int i = 0; i < lightCount; i++) {
    int y = GRID_Y + 34 + i * 18;
    uint16_t c = pocketColor(lightNum[i]);

    tft.fillRoundRect(96, y - 8, 30, 16, 3, c);
    tft.drawRoundRect(96, y - 8, 30, 16, 3, C_GOLD);
    numC(lightNum[i], 111, y, 1, pocketTextColor(lightNum[i]));

    char m[12];
    snprintf(m, sizeof m, "x%d", lightMult[i]);
    textAt(m, 140, y - 8, 2, C_GOLD);
  }

  // A quick rising zap per struck number.
  for (int i = 0; i < lightCount; i++) {
    ledSet(false, false, true);
    blip(900 + i * 300, 60);
    ledOff();
    delay(60);
  }
  delay(900);
}

// ------------------------------------------------------------- Wheel view ---
// A STATIC ring of 38 pockets with a ball orbiting inside it. The ring is
// drawn once per spin; each frame only erases and redraws the ball, which
// makes this cheaper per frame than the scrolling strip.
//
// A real counter-rotating wheel was considered and rejected: 38 pockets on
// a ring this size gives each about 8px of rim, too narrow for a two-digit
// number, and Adafruit_GFX cannot rotate text anyway.
#define WH_CX     160
#define WH_CY     ((GRID_Y + PANEL_Y) / 2)
#define WH_R_OUT  62
#define WH_R_IN   48
#define WH_R_BALL 40          // ball orbits inside the ring, over plain black
#define WH_BALL_R  4
#define TWO_PI_F  6.28318531f

// Centre angle of pocket i, in radians, measured from straight up.
float pocketAngle(int i) {
  return -1.5707963f + (i + 0.5f) * (TWO_PI_F / 38.0f);
}

void drawWheelRing() {
  tft.fillRect(0, GRID_Y - 2, SCR_W, PANEL_Y - GRID_Y + 2, C_BLACK);

  float step = TWO_PI_F / 38.0f;
  for (int i = 0; i < 38; i++) {
    float a0 = -1.5707963f + i * step;
    float a1 = a0 + step;
    uint16_t c = pocketColor(WHEEL[i]);

    int x0i = WH_CX + (int)(cosf(a0) * WH_R_IN),  y0i = WH_CY + (int)(sinf(a0) * WH_R_IN);
    int x1i = WH_CX + (int)(cosf(a1) * WH_R_IN),  y1i = WH_CY + (int)(sinf(a1) * WH_R_IN);
    int x0o = WH_CX + (int)(cosf(a0) * WH_R_OUT), y0o = WH_CY + (int)(sinf(a0) * WH_R_OUT);
    int x1o = WH_CX + (int)(cosf(a1) * WH_R_OUT), y1o = WH_CY + (int)(sinf(a1) * WH_R_OUT);

    // Each pocket is a quad, drawn as two triangles.
    tft.fillTriangle(x0i, y0i, x1i, y1i, x1o, y1o, c);
    tft.fillTriangle(x0i, y0i, x1o, y1o, x0o, y0o, c);

    // Gold tick on a lightning-struck pocket so you can see the target.
    if (lightningOn && lightMultFor(WHEEL[i]) > 0) {
      tft.drawLine(x0i, y0i, x0o, y0o, C_GOLD);
    }
  }
  tft.drawCircle(WH_CX, WH_CY, WH_R_OUT, C_GOLD);
  tft.drawCircle(WH_CX, WH_CY, WH_R_IN,  C_GOLD);
}

void drawBallAt(float ang, uint16_t col) {
  int bx = WH_CX + (int)(cosf(ang) * WH_R_BALL);
  int by = WH_CY + (int)(sinf(ang) * WH_R_BALL);
  tft.fillCircle(bx, by, WH_BALL_R, col);
}

// Spin length varies per spin so no two look alike. Roughly 7 seconds.
#define SPIN_MS_MIN  6600
#define SPIN_MS_MAX  7400
#define DETAIL_AFTER 0.55f   // fraction of the spin after which numbers appear
#define BOUNCE_FROM  0.70f   // fraction after which the ball starts settling
#define BOUNCE_PX    30.0f   // how far it rocks between neighbouring pockets

// Shared motion curve for both animations: quartic ease-out with a decaying
// wobble at the end. The (1-q)^2 envelope reaches exactly zero, so the
// wobble is pure theatre and can never change the result.
float spinProgress(float p, float bouncePx, float* wobbleOut) {
  float inv = 1.0f - p;
  float eased = 1.0f - inv * inv * inv * inv;
  float wobble = 0.0f;
  if (p > BOUNCE_FROM) {
    float q = (p - BOUNCE_FROM) / (1.0f - BOUNCE_FROM);
    float decay = (1.0f - q) * (1.0f - q);
    wobble = sinf(q * 3.14159f * 3.0f) * decay * bouncePx;
  }
  *wobbleOut = wobble;
  return eased;
}

uint8_t spinAnimStrip(int winIdx, int spinMs, int laps, int jitter) {
  long targetOffset = (long)winIdx * TILE_W + (TILE_W - 2) / 2 - 160 + jitter;
  while (targetOffset < 0) targetOffset += WHEEL_PX;
  targetOffset %= WHEEL_PX;

  long total = (long)WHEEL_PX * laps + targetOffset;
  unsigned long start = millis();
  int lastTile = -1;

  for (;;) {
    unsigned long el = millis() - start;
    if (el >= (unsigned long)spinMs) break;
    float p = (float)el / (float)spinMs;

    float wobble;
    float eased = spinProgress(p, BOUNCE_PX, &wobble);
    long travelled = (long)(total * eased + wobble);
    if (travelled < 0) travelled = 0;
    int offset = (int)(travelled % WHEEL_PX);

    bool detail = p > DETAIL_AFTER;
    drawStrip(offset, detail);

    if (detail) {
      int tile = ((offset + 160) / TILE_W) % 38;
      if (tile != lastTile) { sTick(); lastTile = tile; }
    }
  }

  drawStrip((int)targetOffset, true);
  return WHEEL[winIdx];
}

uint8_t spinAnimWheel(int winIdx, int spinMs, int laps, int jitter) {
  drawWheelRing();

  // Land the ball on the winning pocket, nudged within it by the jitter.
  float pocketArc = TWO_PI_F / 38.0f;
  float jitterRad = (jitter / 10.0f) * (pocketArc * 0.30f);
  float startAng  = -1.5707963f;
  float targetAng = pocketAngle(winIdx) + jitterRad;

  // Always travel forwards a whole number of laps plus the gap to target.
  float delta = targetAng - startAng;
  while (delta < 0) delta += TWO_PI_F;
  float total = laps * TWO_PI_F + delta;

  unsigned long start = millis();
  float prevAng = startAng;
  int lastPocket = -1;

  for (;;) {
    unsigned long el = millis() - start;
    if (el >= (unsigned long)spinMs) break;
    float p = (float)el / (float)spinMs;

    float wobble;
    // Wobble expressed in radians: ~a third of a pocket at peak.
    float eased = spinProgress(p, pocketArc * 0.35f, &wobble);
    float ang = startAng + total * eased + wobble;

    drawBallAt(prevAng, C_BLACK);       // erase
    drawBallAt(ang, C_WHITE);           // draw
    prevAng = ang;

    // Tick each time the ball crosses into a new pocket.
    float norm = ang - startAng;
    while (norm < 0) norm += TWO_PI_F;
    int pocket = ((int)(norm / pocketArc)) % 38;
    if (pocket != lastPocket) { sTick(); lastPocket = pocket; }
  }

  drawBallAt(prevAng, C_BLACK);
  drawBallAt(targetAng, C_WHITE);

  // The number is unreadable on an 8px rim, so it goes in the middle where
  // there's room for it.
  uint8_t n = WHEEL[winIdx];
  tft.fillCircle(WH_CX, WH_CY, WH_R_IN - 8, C_BLACK);
  tft.fillCircle(WH_CX, WH_CY, 26, pocketColor(n));
  tft.drawCircle(WH_CX, WH_CY, 26, C_WHITE);
  numC(n, WH_CX, WH_CY, 3, pocketTextColor(n));
  return n;
}

uint8_t spinWheel() {
  int winIdx = esp_random() % 38;

  ledOff();
  tft.fillRect(0, GRID_Y - 2, SCR_W, PANEL_Y - GRID_Y + 2, C_BLACK);
  textC("No more bets!", 160, PANEL_Y - 14, 2, C_GOLD);

  rollLightning();
  drawLightningReveal();

  sSpinUp();

  // Three independent sources of variation, so the same number never lands
  // the same way twice: how long it runs, how many laps, and exactly where
  // in the pocket it comes to rest.
  int spinMs = SPIN_MS_MIN + (int)(esp_random() % (SPIN_MS_MAX - SPIN_MS_MIN + 1));
  int laps   = 3 + (int)(esp_random() % 3);
  int jitter = (int)(esp_random() % 21) - 10;

  if (useWheel) return spinAnimWheel(winIdx, spinMs, laps, jitter);
  return spinAnimStrip(winIdx, spinMs, laps, jitter);
}

// ----------------------------------------------------------------- Payout ---
// Returns the total handed back to the player (stake included on wins).
int settle(uint8_t win) {
  int ret = 0;

  if (betNum[win] > 0) {
    if (lightningOn) {
      // Struck numbers pay their multiplier; everything else pays 29:1
      // instead of 35:1, which is what funds the big hits.
      int m = lightMultFor(win);
      ret += betNum[win] * (m > 0 ? m : LIGHT_PLAIN_RETURN);
    } else {
      ret += betNum[win] * 36;                      // straight up, 35:1
    }
  }

  // Everything below dies on a green.
  if (!isGreen(win)) {
    for (int i = 0; i < 3; i++) {
      if (betDozen[i] == 0) continue;
      int lo = i * 12 + 1, hi = lo + 11;
      if (win >= lo && win <= hi) ret += betDozen[i] * 3;   // 2:1
    }
    bool red  = isRed(win);
    bool even = (win % 2) == 0;
    bool high = win >= 19;

    if ( red)  ret += betOutside[OB_RED]   * 2;
    if (!red)  ret += betOutside[OB_BLACK] * 2;
    if ( even) ret += betOutside[OB_EVEN]  * 2;
    if (!even) ret += betOutside[OB_ODD]   * 2;
    if ( high) ret += betOutside[OB_HIGH]  * 2;
    if (!high) ret += betOutside[OB_LOW]   * 2;
  }

  return ret;
}

void showResult(uint8_t win, int net) {
  tft.fillRect(0, GRID_Y - 2, SCR_W, PANEL_Y - GRID_Y + 2, C_BLACK);

  uint16_t c = pocketColor(win);
  tft.fillRoundRect(120, GRID_Y + 4, 80, 52, 6, c);
  tft.drawRoundRect(120, GRID_Y + 4, 80, 52, 6, C_WHITE);
  numC(win, 160, GRID_Y + 30, 5, pocketTextColor(win));

  const char* word;
  if      (win == ZERO_B) word = "GOLD";     // still a zero, just gold-suited
  else if (win == ZERO_A) word = "GREEN";
  else                    word = isRed(win) ? "RED" : "BLACK";
  textC(word, 160, GRID_Y + 68, 2, C_WHITE);

  char buf[32];
  uint16_t col;
  if (net > 0) {
    col = C_WIN;
    snprintf(buf, sizeof buf, "You win $%d", net);
  } else if (net < 0) {
    col = C_LOSE;
    snprintf(buf, sizeof buf, "You lose $%d", -net);
  } else {
    col = C_WHITE;
    snprintf(buf, sizeof buf, "Even");
  }
  textC(buf, 160, GRID_Y + 90, 2, col);
  textC("Tap screen, or send any key (try x) to continue",
        160, GRID_Y + 110, 1, C_GREY);
}

void doSpin() {
  int staked = totalStaked();
  uint8_t win = spinWheel();

  int ret = settle(win);
  bankroll += ret;
  int net = ret - staked;

  houseTake -= net;              // player's net loss is the house's gain
  saveHouse();                   // every spin; nothing lost to a yanked plug

  if (currentUser >= 0) {
    userWallet[currentUser] = bankroll;   // the wallet IS the live bankroll
    saveUser(currentUser);
  }

  pushHistory(win);
  saveLastBet();                 // capture for REBET before the table clears
  clearBets();

  Serial.printf("Spin: %d  staked %d  returned %d  net %+d  bankroll %d\n",
                win, staked, ret, net, bankroll);

  drawHeader();
  drawHistory();
  showResult(win, net);

  // Green when you profit, red when you don't, blue on a push.
  bool lr = net < 0, lg = net > 0, lb = net == 0;
  ledSet(lr, lg, lb);
  if      (net > 0) sWin();
  else if (net < 0) sLose();
  else              sPush();
  for (int i = 0; i < 2; i++) {
    ledOff();          delay(90);
    ledSet(lr, lg, lb); delay(90);
  }

  waitForTap();
  ledOff();

  if (bankroll < CHIPS[0]) {
    if (currentUser >= 0) giveRefill(currentUser, true);
    resetGame();
    return;
  }

  drawAll();
  drawCursorBox();
}

// ----------------------------------------------------------- Screen flow ---
// A bust still gives a free refill to keep playing, and that refill updates
// the wallet -- but NOT the all-time figure, which only moves from real
// bets and voluntary cash-outs. That's the whole point: the wallet is a
// play-money convenience, the all-time figure on the leaderboard is the
// honest answer to "am I up or down", and a free refill can't launder it.
void resetGame() {
  // bankroll and userWallet are already set by giveRefill() before this is
  // called (bust, or a broke login) -- this just resets the table state.
  clearBets();
  histCount = 0;
  state = ST_BET;
  drawAll();
  drawCursorBox();
}

void openScores() {
  saveHouse();
  if (currentUser >= 0) saveUser(currentUser);
  state = ST_SCORES;
  drawScoresScreen();
}

void handleTapScores(int x, int y) {
  if (inZone(Z_TAB1, x, y))      { lbTab = 0; sClick(); drawScoresScreen(); }
  else if (inZone(Z_TAB2, x, y)) { lbTab = 1; sClick(); drawScoresScreen(); }
  else if (inZone(Z_MYACCT, x, y)) { sClick(); state = ST_ACCOUNT; drawAccountScreen(); }
  else if (inZone(Z_LBBACK, x, y)) { sClick(); state = ST_BET; drawAll(); drawCursorBox(); }
}

void handleTapAccount(int x, int y) {
  if (inZone(Z_CASHOUT, x, y)) {
    if (bankroll > 0) doCashOut();
  } else if (inZone(Z_VOL, x, y)) {
    cycleVolume(); drawAccountScreen();
  } else if (inZone(Z_WHEEL, x, y)) {
    useWheel = !useWheel; prefs.putBool("wheel", useWheel);
    sClick(); drawAccountScreen();
  } else if (inZone(Z_LIGHT, x, y)) {
    lightningOn = !lightningOn; prefs.putBool("lit", lightningOn);
    sClick(); drawAccountScreen();
  } else if (inZone(Z_DELETE, x, y)) {
    sClick(); state = ST_CONFIRM_DELETE; drawConfirmDelete();
  } else if (inZone(Z_LOGOUT, x, y)) {
    sClick(); currentUser = -1; nameLen = 0; nameBuf[0] = 0;
    state = ST_LOGIN_NAME; drawLoginName();
  } else if (inZone(Z_ACCBACK, x, y)) {
    sClick(); state = ST_SCORES; drawScoresScreen();
  }
}

void handleTapConfirmDelete(int x, int y) {
  if (inZone(Z_DELYES, x, y)) { doDeleteCurrentUser(); }
  else if (inZone(Z_DELNO, x, y)) { sClick(); state = ST_ACCOUNT; drawAccountScreen(); }
}

void handleTapLoginName(int x, int y) {
  if (x < KB_X0 || y < KB_Y0) return;
  int c = (x - KB_X0) / KB_CW, r = (y - KB_Y0) / KB_CH;
  if (c >= KB_COLS || r >= KB_ROWS) return;
  int idx = r * KB_COLS + c;
  if      (idx < KB_NCHARS) typeChar(KB_CHARS[idx]);
  else if (idx == KB_SPC)   typeChar(' ');
  else if (idx == KB_DEL)   backspace();
  else                      submitName();
}

void handleTapPin(int x, int y, bool isNew) {
  if (x < PIN_X0 || y < PIN_Y0) return;
  int c = (x - PIN_X0) / PIN_CW, r = (y - PIN_Y0) / PIN_CH;
  if (c >= 3 || r >= 4) return;
  int idx = r * 3 + c;
  const char* lbl = PIN_LAYOUT[idx];
  if      (!strcmp(lbl, "DEL")) pinBackspace();
  else if (!strcmp(lbl, "OK"))  { if (isNew) submitNewPin(); else submitLoginPin(); }
  else                          pinDigit(lbl[0]);
}

void handleTapBetting(int x, int y) {
  if (inZone(Z_BANK, x, y)) { sClick(); openScores(); return; }
  if (inZone(Z_SPIN, x, y)) {
    if (totalStaked() > 0) doSpin();
  } else if (inZone(Z_REBET, x, y)) {
    sClick(); doRebet();
  } else if (inZone(Z_2X, x, y)) {
    if (totalStaked() > 0) { sClick(); doDoubleBets(); }
  } else if (inZone(Z_CLEAR, x, y)) {
    bankroll += totalStaked();
    clearBets();
    drawHeader(); drawTable(); drawActionButtons(); sClick();
  } else if (inZone(Z_MINUS, x, y)) {
    if (chipIdx > 0) { chipIdx--; drawChipValue(); sClick(); }
  } else if (inZone(Z_PLUS, x, y)) {
    if (chipIdx < maxChipIdx()) { chipIdx++; drawChipValue(); sClick(); }
  } else {
    if (placeBet(x, y)) { drawHeader(); drawActionButtons(); sChip(); }
  }
}

// ------------------------------------------------------------------ Setup ---
void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  ledOff();

  ledcAttach(SPEAKER_PIN, 2000, TONE_RES_BITS);

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(1);            // landscape, 320 x 240
  tft.setTextWrap(false);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  prefs.begin("roulette", false);
  volLevel    = prefs.getInt("vol", VOL_MED);
  useWheel    = prefs.getBool("wheel", false);
  lightningOn = prefs.getBool("lit", false);
  loadUsers();
  houseTake = prefs.getLong("house", 0);
  houseSaved = houseTake;

  nameLen = 0; nameBuf[0] = 0;
  state = ST_LOGIN_NAME;
  drawLoginName();

  Serial.println("CYD Roulette ready.");
  Serial.println("No touchscreen? Drive it from here:");
  Serial.println("  Login: type your name, Enter. Then your 4-digit PIN, Enter.");
  Serial.println("  Table: w/a/s/d move  p place  g spin  c clear  - + chip");
  Serial.println("         k scores  v volume");
}

void loop() {
  while (Serial.available()) handleKey((char)Serial.read());

  int x, y;
  if (getTap(x, y)) {
    switch (state) {
      case ST_LOGIN_NAME: handleTapLoginName(x, y);       break;
      case ST_LOGIN_PIN:  handleTapPin(x, y, false);       break;
      case ST_NEW_PIN:    handleTapPin(x, y, true);        break;
      case ST_BET:        handleTapBetting(x, y);          break;
      case ST_SCORES:     handleTapScores(x, y);           break;
      case ST_ACCOUNT:    handleTapAccount(x, y);          break;
      case ST_CONFIRM_DELETE: handleTapConfirmDelete(x, y); break;
    }
  }
  delay(20);
}
