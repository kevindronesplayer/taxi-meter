/*
  Taxi meter firmware for M5Stack Core2 with a GPS unit (AT6668, M5Stack
  "GPS Unit v1.1") wired to Port A.

  Port A is normally I2C (SDA/SCL), but on Core2 it's just two plain
  GPIOs (G32/G33) behind a Grove connector, and the GPS unit only cares
  about its own TX/RX lines, not I2C semantics -- so it works fine there
  as a UART, it just needs the sketch pointed at G32/G33 instead of the
  official UART Port C (G13/G14). 9600 baud, standard NMEA output.

  If the on-screen "RX:" counter stays 0, the Grove cable's two signal
  wires may be swapped relative to what this sketch expects -- try
  swapping GPS_RX_PIN/GPS_TX_PIN below before assuming it's a dead unit.

  Libraries (install via Arduino Library Manager):
    - M5Unified
    - M5GFX          (pulled in automatically by M5Unified)
    - TinyGPSPlus

  Board: "M5Stack-Core2" in the ESP32 boards package.

  Fare logic mirrors the mobile web version: base fare includes the first
  X km, then a flat step every 200m; a low-speed (<5 km/h) "waiting" fee
  accrues per N seconds; night surcharge is either a flat add-on or a
  percentage, depending on region; a highway toggle adds a flat toll per
  tap. Distance/speed come from live GPS fixes instead of a simulator.

  On-screen labels are English-only: the bundled GFX fonts only cover a
  GB2312 (Simplified Chinese) subset, which is missing most Traditional
  Chinese glyphs used in Taiwan, so Traditional Chinese text would render
  as blank boxes without loading an external CJK font file from SD card.
*/

#include <M5Unified.h>
#include <TinyGPSPlus.h>
#include <math.h>
#include <string.h>

// ---------------- GPS ----------------
static const int GPS_RX_PIN = 33; // Port A: Core2 RX <- GPS TX
static const int GPS_TX_PIN = 32; // Port A: Core2 TX -> GPS RX (unused, we never write to the GPS)
static const uint32_t GPS_BAUD = 9600;
TinyGPSPlus gps;

// ---------------- Fare regions ----------------
struct Region {
  const char* name;
  int base;
  float includedKm;
  int stepM;
  int stepFare;
  int waitSec;
  int waitFare;
  bool hasNight;
  bool nightIsPercent;
  int nightAmount;
};

Region REGIONS[] = {
  { "Taipei/NewTaipei", 85, 1.25f, 200, 5, 60,  5, true,  false, 20 },
  { "Taoyuan",           90, 1.25f, 200, 5, 80,  5, false, false, 0  },
  { "Taichung",          85, 1.25f, 200, 5, 60,  5, true,  false, 20 },
  { "Tainan",            85, 1.25f, 200, 5, 100, 5, true,  true,  20 },
  { "Kaohsiung",         85, 1.25f, 200, 5, 100, 5, true,  true,  20 },
};
const int REGION_COUNT = 5;
int regionIdx = 0;
inline Region& currentRegion() { return REGIONS[regionIdx]; }

const int HIGHWAY_FEE = 40;
const double WAIT_SPEED_KMH = 5.0;

// ---------------- Trip state ----------------
enum Status { VACANT, RUNNING, STOPPED };
Status status = VACANT;

double distanceKm = 0;
unsigned long waitingSec = 0;
unsigned long elapsedSec = 0;
int tollCount = 0;
double lastSpeedKmh = 0;
bool nightNow = false;

bool haveLastFix = false;
double lastLat = 0, lastLng = 0;

unsigned long lastSecondMillis = 0;
unsigned long resetArmedAt = 0; // 0 = not armed

// ---------------- UI layout ----------------
struct Btn {
  int x;
  int y;
  int w;
  int h;
  const char* l1;
  const char* l2;
};
Btn buttons[5];
const int BTN_BAR_H = 46;

// Off-screen buffer: everything is drawn into this sprite and pushed to
// the real panel in one shot, so the panel never shows a half-drawn
// frame (which is what read as screen flicker).
M5Canvas canvas(&M5.Display);

uint16_t colBg, colScreen, colScreenNight, colInk, colInkNight, colRed, colRedDim,
         colGreen, colGreenDim, colPanel, colAmber, colWhite, colMuted;

void initColors() {
  colBg          = M5.Display.color565(10, 10, 10);
  colScreen      = M5.Display.color565(120, 200, 80);
  colScreenNight = M5.Display.color565(40, 55, 42);
  colInk         = M5.Display.color565(18, 33, 12);
  colInkNight    = M5.Display.color565(190, 230, 170);
  colRed         = M5.Display.color565(192, 57, 43);
  colRedDim      = M5.Display.color565(90, 32, 25);
  colGreen       = M5.Display.color565(45, 106, 45);
  colGreenDim    = M5.Display.color565(30, 60, 30);
  colPanel       = M5.Display.color565(38, 38, 38);
  colAmber       = M5.Display.color565(232, 185, 59);
  colWhite       = M5.Display.color565(238, 238, 238);
  colMuted       = M5.Display.color565(154, 154, 154);
}

void layoutButtons() {
  int w = M5.Display.width();
  int h = M5.Display.height();
  int bw = w / 5;
  const char* l1[5] = { "F1", "F2", "F3", "F4", "F5" };
  const char* l2[5] = { "VACANT", "START", "STOP", "HWY +40", "RECEIPT" };
  for (int i = 0; i < 5; i++) {
    buttons[i] = { i * bw, h - BTN_BAR_H, bw - 1, BTN_BAR_H, l1[i], l2[i] };
  }
}

// ---------------- Fare math ----------------
long computeFare() {
  Region& r = currentRegion();
  double overKm = distanceKm - r.includedKm;
  if (overKm < 0) overKm = 0;
  long distFare = (long)(overKm * 1000.0 / r.stepM) * r.stepFare;
  long waitFare = (long)(waitingSec / (unsigned long)r.waitSec) * r.waitFare;
  return r.base + distFare + waitFare;
}

long computeNight(long fare) {
  Region& r = currentRegion();
  if (!r.hasNight || !nightNow) return 0;
  if (!r.nightIsPercent) return r.nightAmount;
  return (long)round(fare * r.nightAmount / 100.0);
}

void formatHMS(unsigned long totalSec, char* buf) {
  unsigned long h = totalSec / 3600;
  unsigned long m = (totalSec % 3600) / 60;
  unsigned long s = totalSec % 60;
  sprintf(buf, "%02lu:%02lu:%02lu", h, m, s);
}

void formatMS(unsigned long totalSec, char* buf) {
  unsigned long m = totalSec / 60;
  unsigned long s = totalSec % 60;
  sprintf(buf, "%02lu:%02lu", m, s);
}

// ---------------- GPS handling ----------------
void updateNightFromGps() {
  if (!gps.time.isValid() || !gps.time.isUpdated()) return;
  int utcHour = gps.time.hour();
  int localHour = (utcHour + 8) % 24; // Taiwan = UTC+8
  nightNow = (localHour >= 23 || localHour < 6);
}

void gpsTick() {
  while (Serial2.available()) {
    gps.encode(Serial2.read());
  }

  updateNightFromGps();

  if (!gps.location.isUpdated() || !gps.location.isValid()) return;

  double lat = gps.location.lat();
  double lng = gps.location.lng();

  double speedKmh = gps.speed.isValid() ? gps.speed.kmph() : lastSpeedKmh;

  if (haveLastFix) {
    double deltaM = TinyGPSPlus::distanceBetween(lastLat, lastLng, lat, lng);
    if (status == RUNNING) {
      distanceKm += deltaM / 1000.0;
    }
  }

  lastLat = lat;
  lastLng = lng;
  haveLastFix = true;
  lastSpeedKmh = speedKmh;
}

// ---------------- Trip control ----------------
void resetTrip() {
  distanceKm = 0;
  waitingSec = 0;
  elapsedSec = 0;
  tollCount = 0;
  resetArmedAt = 0;
}

void onVacant() {
  if (status == RUNNING) {
    unsigned long now = millis();
    if (resetArmedAt != 0 && now - resetArmedAt < 3000) {
      status = VACANT;
      resetTrip();
    } else {
      resetArmedAt = now; // tap again within 3s to confirm
    }
    return;
  }
  status = VACANT;
  resetTrip();
}

void onStart() {
  if (status != VACANT) return;
  resetTrip();
  status = RUNNING;
}

void onStop() {
  if (status != RUNNING) return;
  status = STOPPED;
}

void onHighway() {
  if (status != RUNNING) return;
  tollCount++;
}

bool showingReceipt = false;

void onReceipt() {
  showingReceipt = !showingReceipt;
}

void onRegionTap() {
  if (status != VACANT) return; // avoid changing rates mid-trip
  regionIdx = (regionIdx + 1) % REGION_COUNT;
}

// ---------------- Drawing ----------------
void drawButton(int idx, uint16_t bg, bool dim) {
  int x = buttons[idx].x, y = buttons[idx].y, w = buttons[idx].w, h = buttons[idx].h;
  canvas.fillRect(x, y, w, h, bg);
  canvas.setTextDatum(top_center);
  canvas.setTextColor(dim ? colMuted : colWhite, bg);
  canvas.setTextSize(1);
  canvas.drawString(buttons[idx].l1, x + w / 2, y + 3);
  canvas.drawString(buttons[idx].l2, x + w / 2, y + 16);
}

void drawButtons() {
  drawButton(0, colRed, false);
  drawButton(1, status == VACANT ? colGreen : colGreenDim, status != VACANT);
  drawButton(2, status == RUNNING ? colRedDim : colPanel, status != RUNNING);
  drawButton(3, colPanel, status != RUNNING);
  drawButton(4, colPanel, false);
}

void drawScreen() {
  int w = M5.Display.width();
  int topH = 20;
  int screenY = topH + 2;
  int screenH = M5.Display.height() - BTN_BAR_H - screenY - 2;

  uint16_t bg = nightNow ? colScreenNight : colScreen;
  uint16_t ink = nightNow ? colInkNight : colInk;

  // top status bar
  canvas.fillRect(0, 0, w, topH, colBg);
  canvas.setTextDatum(top_left);
  canvas.setTextColor(colMuted, colBg);
  canvas.setTextSize(1);
  const char* statusLabel = status == VACANT ? "VACANT" : (status == RUNNING ? "ON TRIP" : "STOPPED");
  canvas.drawString(statusLabel, 4, 5);

  // battery %, pinned to the very top-right corner
  int batt = M5.Power.getBatteryLevel();
  char battBuf[8];
  if (batt >= 0) sprintf(battBuf, "%d%%", batt); else strcpy(battBuf, "--");
  canvas.setTextDatum(top_right);
  canvas.setTextColor(colWhite, colBg);
  canvas.drawString(battBuf, w - 4, 5);
  int battW = canvas.textWidth(battBuf);

  // GPS fix indicator, just to the left of the battery reading
  bool fix = gps.location.isValid();
  canvas.setTextColor(fix ? colGreen : colRed, colBg);
  canvas.drawString(fix ? "GPS FIX" : "NO FIX", w - 4 - battW - 10, 5);

  // main LCD-style panel
  canvas.fillRoundRect(2, screenY, w - 4, screenH, 8, bg);

  canvas.setTextDatum(top_left);
  canvas.setTextColor(ink, bg);
  canvas.setTextSize(1);
  canvas.drawString(currentRegion().name, 10, screenY + 6);

  char hms[10];
  formatHMS(elapsedSec, hms);
  canvas.setTextSize(3);
  canvas.drawString(hms, 10, screenY + 26);

  // GPS diagnostics: RX = bytes seen from the module at all, OK = sentences
  // that parsed with a valid checksum, FAIL = sentences with a checksum
  // mismatch (garbled), SAT = satellites currently tracked. RX growing
  // with OK stuck and FAIL climbing means bytes are arriving corrupted --
  // usually a noisy/loose wire, not a baud or pin problem.
  char gdbg[48];
  sprintf(gdbg, "RX:%lu OK:%lu FAIL:%lu SAT:%d", (unsigned long)gps.charsProcessed(),
          (unsigned long)gps.passedChecksum(), (unsigned long)gps.failedChecksum(),
          (int)gps.satellites.value());
  canvas.setTextDatum(top_left);
  canvas.setTextColor(ink, bg);
  canvas.setTextSize(1);
  canvas.drawString(gdbg, 10, screenY + 54);

  // fare, big, right aligned
  char fareBuf[16];
  sprintf(fareBuf, "%ld", computeFare());
  canvas.setTextDatum(top_right);
  canvas.setTextSize(1);
  canvas.drawString("FARE", w - 14, screenY + 4);
  canvas.setTextSize(6);
  canvas.drawString(fareBuf, w - 14, screenY + 16);

  // four mini cells
  int cellY = screenY + 70;
  int cellW = (w - 16) / 4;
  const char* labels[4] = { "DIST km", "WAIT", "TOLL", "NIGHT" };
  char v0[8], v1[8], v2[8], v3[8];
  sprintf(v0, "%.1f", distanceKm);
  formatMS(waitingSec, v1);
  sprintf(v2, "%d", tollCount * HIGHWAY_FEE);
  sprintf(v3, "%ld", computeNight(computeFare()));
  const char* vals[4] = { v0, v1, v2, v3 };

  for (int i = 0; i < 4; i++) {
    int cx = 8 + i * cellW;
    canvas.setTextDatum(top_left);
    canvas.setTextColor(ink, bg);
    canvas.setTextSize(1);
    canvas.drawString(labels[i], cx, cellY);
    canvas.setTextSize(3);
    canvas.drawString(vals[i], cx, cellY + 12);
  }

  if (resetArmedAt != 0 && status == RUNNING) {
    canvas.setTextDatum(top_left);
    canvas.setTextColor(colAmber, bg);
    canvas.setTextSize(1);
    canvas.drawString("TAP F1 AGAIN TO RESET", 10, screenY + screenH - 12);
  }

  drawButtons();
}

void drawReceipt() {
  int w = M5.Display.width();
  int h = M5.Display.height();
  canvas.fillRect(20, 20, w - 40, h - 40, colWhite);
  canvas.drawRect(20, 20, w - 40, h - 40, colInk);

  long fare = computeFare();
  long night = computeNight(fare);
  long toll = tollCount * HIGHWAY_FEE;
  long total = fare + night + toll;

  int x = 34, y = 32, lh = 18;
  canvas.setTextDatum(top_left);
  canvas.setTextColor(colInk, colWhite);
  canvas.setTextSize(2);
  canvas.drawString("RECEIPT", x, y); y += lh + 6;
  canvas.setTextSize(1);
  canvas.drawString(currentRegion().name, x, y); y += lh;
  char hms[10]; formatHMS(elapsedSec, hms);
  char line[48];
  sprintf(line, "Trip time: %s", hms); canvas.drawString(line, x, y); y += lh;
  sprintf(line, "Distance: %.2f km", distanceKm); canvas.drawString(line, x, y); y += lh;
  sprintf(line, "Fare:   $%ld", fare); canvas.drawString(line, x, y); y += lh;
  sprintf(line, "Toll:   $%ld", toll); canvas.drawString(line, x, y); y += lh;
  sprintf(line, "Night:  $%ld", night); canvas.drawString(line, x, y); y += lh + 4;
  canvas.setTextSize(2);
  sprintf(line, "TOTAL: $%ld", total); canvas.drawString(line, x, y);

  canvas.setTextDatum(bottom_center);
  canvas.setTextSize(1);
  canvas.setTextColor(colMuted, colWhite);
  canvas.drawString("tap anywhere to close", w / 2, h - 26);
}

// ---------------- Touch ----------------
bool pointIn(int idx, int x, int y) {
  Btn& b = buttons[idx];
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;

  if (showingReceipt) {
    showingReceipt = false;
    return;
  }

  if (pointIn(0, t.x, t.y)) { onVacant(); return; }
  if (pointIn(1, t.x, t.y)) { onStart(); return; }
  if (pointIn(2, t.x, t.y)) { onStop(); return; }
  if (pointIn(3, t.x, t.y)) { onHighway(); return; }
  if (pointIn(4, t.x, t.y)) { onReceipt(); return; }

  // tap the top-left status area to cycle region while vacant
  if (t.y < 20 && t.x < M5.Display.width() / 2) { onRegionTap(); return; }
}

// ---------------- Arduino entry points ----------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setColorDepth(16);
  initColors();
  layoutButtons();

  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.fillSprite(colBg);

  Serial2.setRxBufferSize(1024); // headroom in case a draw call briefly delays draining
  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  lastSecondMillis = millis();
  drawScreen();
  canvas.pushSprite(0, 0);
}

void loop() {
  M5.update();
  gpsTick();
  handleTouch();

  unsigned long now = millis();
  if (now - lastSecondMillis >= 1000) {
    lastSecondMillis += 1000;
    if (status == RUNNING) {
      elapsedSec++;
      if (lastSpeedKmh < WAIT_SPEED_KMH) waitingSec++;
    }
    if (resetArmedAt != 0 && now - resetArmedAt > 3000) resetArmedAt = 0;
  }

  static unsigned long lastDraw = 0;
  if (now - lastDraw >= 200) {
    lastDraw = now;
    if (showingReceipt) drawReceipt(); else drawScreen();
    canvas.pushSprite(0, 0);
  }
}
