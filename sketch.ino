// ---------------------------------------------------------------------------
// CYD Pipeline Test
//
// Tap the box, it changes color and counts up.
// If this runs, the whole toolchain works and we can start building the game.
// ---------------------------------------------------------------------------

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

TFT_eSPI tft = TFT_eSPI();

// The backlight pin. Some setups of TFT_eSPI define this for us; if not,
// GPIO 21 is where it lives on the Cheap Yellow Display.
#ifndef TFT_BL
  #define TFT_BL 21
#endif

// ------------------------------------------------------------------ Touch ---
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// Raw range the touch panel reports. If taps land in the wrong spot,
// these are the numbers to adjust.
#define RAW_X_MIN 200
#define RAW_X_MAX 3700
#define RAW_Y_MIN 240
#define RAW_Y_MAX 3800
#define TOUCH_MIN_PRESSURE 300   // ignores feather-light phantom touches

SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

// Returns true only on the FIRST moment of a new press, so holding your
// finger down doesn't fire a hundred times per second.
bool getTap(int &x, int &y) {
  static bool wasDown = false;
  bool isDown = ts.touched();
  bool fresh = false;

  if (isDown && !wasDown) {
    TS_Point p = ts.getPoint();
    if (p.z >= TOUCH_MIN_PRESSURE) {
      x = map(p.x, RAW_X_MIN, RAW_X_MAX, 0, tft.width());
      y = map(p.y, RAW_Y_MIN, RAW_Y_MAX, 0, tft.height());
      x = constrain(x, 0, tft.width()  - 1);
      y = constrain(y, 0, tft.height() - 1);
      fresh = true;
    } else {
      isDown = false;
    }
  }

  wasDown = isDown;
  return fresh;
}

// ------------------------------------------------------------------- Game ---
// Pretend this is your game. Right now it's a box that changes color.

const uint16_t PALETTE[] = {
  TFT_RED, TFT_ORANGE, TFT_YELLOW, TFT_GREEN, TFT_CYAN, TFT_BLUE, TFT_MAGENTA
};
const int N_COLORS = 7;

int colorIdx = 0;
int tapCount = 0;

// The tappable box: left edge, top edge, width, height.
const int BOX_X = 90, BOX_Y = 80, BOX_W = 140, BOX_H = 90;

bool insideBox(int x, int y) {
  return x >= BOX_X && x < BOX_X + BOX_W &&
         y >= BOX_Y && y < BOX_Y + BOX_H;
}

void drawScreen(int lastX, int lastY) {
  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("TAP THE BOX", 160, 12, 4);

  tft.fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, 8, PALETTE[colorIdx]);
  tft.drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, 8, TFT_WHITE);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_BLACK, PALETTE[colorIdx]);
  tft.drawNumber(tapCount, BOX_X + BOX_W / 2, BOX_Y + BOX_H / 2, 6);

  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  char buf[40];
  if (lastX < 0) snprintf(buf, sizeof buf, "waiting for a tap...");
  else           snprintf(buf, sizeof buf, "last tap: %d, %d", lastX, lastY);
  tft.drawString(buf, 160, 232, 2);
}

// ------------------------------------------------------------------ Setup ---
void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);          // turn the backlight on
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1);               // landscape, 320 x 240

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  drawScreen(-1, -1);
  Serial.println("Pipeline test ready. Tap the screen.");
}

void loop() {
  int x, y;
  if (getTap(x, y)) {
    Serial.printf("Tap at %d, %d\n", x, y);
    if (insideBox(x, y)) {
      colorIdx = (colorIdx + 1) % N_COLORS;
      tapCount++;
    }
    drawScreen(x, y);
  }
  delay(20);
}
