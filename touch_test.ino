// ---------------------------------------------------------------------------
// Touch diagnostic
//
// Answers one question: does the touch chip report anything at all?
// Click on the screen in the simulator and watch the serial output.
// ---------------------------------------------------------------------------

#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_ILI9341.h"
#include <XPT2046_Touchscreen.h>

#define TFT_CS   15
#define TFT_DC    2
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCK  14
#define TFT_BL   21

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC);

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

SPIClass touchSPI = SPIClass(VSPI);

// NOTE: no IRQ pin passed here. This makes the library poll the chip over SPI
// instead of waiting on an interrupt line, which is the more forgiving mode.
XPT2046_Touchscreen ts(XPT2046_CS);

int lastZ = -1;
unsigned long lastBeat = 0;
long samples = 0, nonZero = 0;

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(1);
  tft.setTextWrap(false);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(30, 60);
  tft.print("CLICK ANYWHERE");
  tft.setTextSize(1);
  tft.setCursor(30, 100);
  tft.print("Watch the serial monitor below");

  Serial.println("=== Touch diagnostic ===");
  Serial.println("Click on the screen. Any line starting with HIT means");
  Serial.println("the touch chip is alive. Silence means it is not emulated.");
}

void loop() {
  samples++;

  bool t = ts.touched();
  TS_Point p = ts.getPoint();

  if (t || p.z != 0 || p.x != 0 || p.y != 0) {
    nonZero++;
    Serial.printf("HIT  touched=%d  raw x=%d y=%d z=%d\n", t ? 1 : 0, p.x, p.y, p.z);

    // Draw a dot roughly where it thinks you clicked, using a wide-open
    // mapping so nothing gets filtered out.
    int sx = map(p.x, 200, 3700, 0, 320);
    int sy = map(p.y, 240, 3800, 0, 240);
    sx = constrain(sx, 0, 319);
    sy = constrain(sy, 0, 239);
    tft.fillCircle(sx, sy, 3, ILI9341_GREEN);
    lastZ = p.z;
  }

  // Heartbeat every 3 seconds so you know it is still running.
  if (millis() - lastBeat > 3000) {
    lastBeat = millis();
    Serial.printf("...alive. %ld samples, %ld with any touch data. lastZ=%d\n",
                  samples, nonZero, lastZ);
  }

  delay(20);
}
