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

bool getTap(int &x, int &y) {
  static bool wasDown = false;
  bool isDown = ts.touched();
  bool fresh = false;
  if (isDown && !wasDown) {
    TS_Point p = ts.getPoint();
    if (p.z >= TOUCH_MIN_PRESSURE) {
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

// Continues on either a screen tap or any key typed into the serial monitor.
// Without the serial half, the result screen would hang forever in a
// simulator that has no touch.
void waitForTap() {
  while (ts.touched()) delay(20);
  while (Serial.available()) Serial.read();
  for (;;) {
    if (ts.touched()) { while (ts.touched()) delay(20); return; }
    if (Serial.available()) { while (Serial.available()) Serial.read(); return; }
    delay(20);
  }
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

bool isRed(uint8_t n) {
  switch (n) {
    case  1: case  3: case  5: case  7: case  9: case 12:
    case 14: case 16: case 18: case 19: case 21: case 23:
    case 25: case 27: case 30: case 32: case 34: case 36:
      return true;
  }
  return false;
}

uint16_t pocketColor(uint8_t n) {
  if (isGreen(n)) return C_GREEN;
  return isRed(n) ? C_RED : C_BLACK;
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

const int CHIPS[] = { 1, 5, 25, 100 };
#define N_CHIPS 4
int chipIdx = 1;

int totalStaked() {
  int t = 0;
  for (int i = 0; i < 39; i++) t += betNum[i];
  for (int i = 0; i < 3;  i++) t += betDozen[i];
  for (int i = 0; i < 6;  i++) t += betOutside[i];
  return t;
}

void clearBets() {
  for (int i = 0; i < 39; i++) betNum[i] = 0;
  for (int i = 0; i < 3;  i++) betDozen[i] = 0;
  for (int i = 0; i < 6;  i++) betOutside[i] = 0;
}

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

const Zone Z_MINUS = {   4, 182, 30, 26 };
const Zone Z_PLUS  = {  86, 182, 30, 26 };
const Zone Z_CLEAR = { 124, 182, 84, 26 };
const Zone Z_SPIN  = { 216, 182, 100, 50 };

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
    numC(n, x + (cw - 3) / 2, HIST_Y + HIST_H / 2, 1, C_WHITE);
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
  numC(n, x + w / 2, y + h / 2, isGreen(n) ? 2 : 1, C_WHITE);
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
  tft.fillRect(36, 182, 48, 26, C_BLACK);
  char buf[8];
  snprintf(buf, sizeof buf, "$%d", CHIPS[chipIdx]);
  textC(buf, 60, 195, 2, C_GOLD);
}

void drawPanel() {
  tft.fillRect(0, PANEL_Y, SCR_W, SCR_H - PANEL_Y, C_BLACK);
  drawButton(Z_MINUS, "-", C_WHITE, 2);
  drawButton(Z_PLUS,  "+", C_WHITE, 2);
  drawChipValue();
  drawButton(Z_CLEAR, "CLEAR", C_WHITE, 2);
  drawButton(Z_SPIN,  "SPIN",  C_GOLD, 3);
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

// --------------------------------------------------------- Keyboard play ---
// The simulator has no touch, so the board can also be driven from the serial
// monitor. The cursor stays hidden until the first key arrives, so on real
// hardware -- where nothing is typed -- none of this ever shows up.
void doSpin();          // defined below, called by the key handler

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
}

void printKeyHelp() {
  Serial.println("Keyboard: w/a/s/d move  p place  g spin  c clear  - + chip size");
  Serial.println("(type into the box under the serial monitor, then press Enter)");
}

void handleKey(char k) {
  if (k == '\n' || k == '\r') return;       // Wokwi sends a newline per line
  if (!kbActive) { kbActive = true; drawCursorBox(); printKeyHelp(); }

  switch (k) {
    case 'w': case 'W': moveCursor(0, -1); break;
    case 's': case 'S': moveCursor(0,  1); break;
    case 'a': case 'A': moveCursor(-1, 0); break;
    case 'd': case 'D': moveCursor( 1, 0); break;
    case 'p': case 'P': case ' ': placeAtCursor(); break;
    case 'g': case 'G':
      if (totalStaked() > 0) { doSpin(); drawCursorBox(); }
      break;
    case 'c': case 'C':
      bankroll += totalStaked();
      clearBets();
      drawHeader(); drawTable(); drawCursorBox();
      break;
    case '-': case '_':
      if (chipIdx > 0) { chipIdx--; drawChipValue(); }
      break;
    case '+': case '=':
      if (chipIdx < N_CHIPS - 1) { chipIdx++; drawChipValue(); }
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

void drawStrip(int offset) {
  int cy = STRIP_Y + STRIP_H / 2;
  int first = offset / TILE_W;
  int shift = offset % TILE_W;

  for (int i = -1; i <= SCR_W / TILE_W + 1; i++) {
    int idx = ((first + i) % 38 + 38) % 38;
    uint8_t n = WHEEL[idx];
    int x = i * TILE_W - shift;
    uint16_t c = pocketColor(n);
    tft.fillRect(x, STRIP_Y, TILE_W - 2, STRIP_H, c);
    tft.drawRect(x, STRIP_Y, TILE_W - 2, STRIP_H, C_LINE);
    numC(n, x + (TILE_W - 2) / 2, cy, 3, C_WHITE);
  }

  tft.fillTriangle(160, STRIP_Y - 9, 153, STRIP_Y - 1, 167, STRIP_Y - 1, C_GOLD);
  tft.drawFastVLine(160, STRIP_Y, STRIP_H, C_GOLD);
}

uint8_t spinWheel() {
  int winIdx = esp_random() % 38;

  tft.fillRect(0, GRID_Y - 2, SCR_W, PANEL_Y - GRID_Y + 2, C_BLACK);
  textC("No more bets!", 160, PANEL_Y - 14, 2, C_GOLD);

  // The marker sits at x=160, so the winning tile must finish under it.
  int centerTile = 160 / TILE_W;
  int targetOffset = (winIdx - centerTile) * TILE_W + (160 % TILE_W) - TILE_W / 2;
  while (targetOffset < 0) targetOffset += WHEEL_PX;
  targetOffset %= WHEEL_PX;

  int totalTravel = WHEEL_PX * 4 + targetOffset;
  int travelled = 0;
  float speed = 46.0f;

  while (travelled < totalTravel) {
    int remaining = totalTravel - travelled;
    if (remaining < 1200) speed = 2.0f + (remaining / 1200.0f) * 44.0f;
    if (speed < 2.0f) speed = 2.0f;

    travelled += (int)speed;
    if (travelled > totalTravel) travelled = totalTravel;

    drawStrip(travelled % WHEEL_PX);
    delay(16);
  }

  return WHEEL[winIdx];
}

// ----------------------------------------------------------------- Payout ---
// Returns the total handed back to the player (stake included on wins).
int settle(uint8_t win) {
  int ret = 0;

  if (betNum[win] > 0) ret += betNum[win] * 36;     // straight up, 35:1

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
  numC(win, 160, GRID_Y + 30, 5, C_WHITE);

  const char* word = isGreen(win) ? "GREEN" : (isRed(win) ? "RED" : "BLACK");
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
  textC("Tap to continue", 160, GRID_Y + 110, 1, C_GREY);
}

void doSpin() {
  int staked = totalStaked();
  uint8_t win = spinWheel();

  int ret = settle(win);
  bankroll += ret;
  int net = ret - staked;

  pushHistory(win);
  clearBets();

  Serial.printf("Spin: %d  staked %d  returned %d  net %+d  bankroll %d\n",
                win, staked, ret, net, bankroll);

  drawHeader();
  drawHistory();
  showResult(win, net);
  waitForTap();

  if (bankroll < CHIPS[0]) {
    tft.fillScreen(C_BLACK);
    textC("BUSTED", 160, 96, 4, C_LOSE);
    textC("Tap for a fresh $1000", 160, 140, 2, C_WHITE);
    waitForTap();
    bankroll = 1000;
    histCount = 0;
  }

  drawAll();
}

// ------------------------------------------------------------------ Setup ---
void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(1);            // landscape, 320 x 240
  tft.setTextWrap(false);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  clearBets();
  drawAll();

  Serial.println("CYD Roulette ready.");
  Serial.println("No touchscreen? Drive it from here:");
  Serial.println("  w/a/s/d move  p place  g spin  c clear  - + chip size");
}

void loop() {
  while (Serial.available()) handleKey((char)Serial.read());

  int x, y;
  if (getTap(x, y)) {
    if (inZone(Z_SPIN, x, y)) {
      if (totalStaked() > 0) doSpin();
    } else if (inZone(Z_CLEAR, x, y)) {
      bankroll += totalStaked();
      clearBets();
      drawHeader();
      drawTable();
    } else if (inZone(Z_MINUS, x, y)) {
      if (chipIdx > 0) { chipIdx--; drawChipValue(); }
    } else if (inZone(Z_PLUS, x, y)) {
      if (chipIdx < N_CHIPS - 1) { chipIdx++; drawChipValue(); }
    } else {
      if (placeBet(x, y)) drawHeader();
    }
  }
  delay(20);
}
