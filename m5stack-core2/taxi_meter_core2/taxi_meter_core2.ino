/*
  Taxi meter firmware for M5Stack Core2 with a GPS unit (AT6558D, e.g.
  M5Stack "Mini GPS/BDS Unit") on Port C.

  Wiring: GPS unit -> Port C (Grove UART). Core2 RX (GPIO13) <- GPS TX,
  Core2 TX (GPIO14) -> GPS RX. 9600 baud, standard NMEA output.

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

// ---------------- GPS ----------------
static const int GPS_RX_PIN = 13; // Port C: Core2 RX <- GPS TX
static const int GPS_TX_PIN = 14; // Port C: Core2 TX -> GPS RX
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
void drawButton(const Btn& b, uint16_t bg, bool dim) {
  M5.Display.fillRect(b.x, b.y, b.w, b.h, bg);
  M5.Display.setTextDatum(top_center);
  M5.Display.setTextColor(dim ? colMuted : colWhite, bg);
  M5.Display.setTextSize(1);
  M5.Display.drawString(b.l1, b.x + b.w / 2, b.y + 3);
  M5.Display.drawString(b.l2, b.x + b.w / 2, b.y + 16);
}

void drawButtons() {
  drawButton(buttons[0], colRed, false);
  drawButton(buttons[1], status == VACANT ? colGreen : colGreenDim, status != VACANT);
  drawButton(buttons[2], status == RUNNING ? colRedDim : colPanel, status != RUNNING);
  drawButton(buttons[3], colPanel, status != RUNNING);
  drawButton(buttons[4], colPanel, false);
}

void drawScreen() {
  int w = M5.Display.width();
  int topH = 20;
  int screenY = topH + 2;
  int screenH = M5.Display.height() - BTN_BAR_H - screenY - 2;

  uint16_t bg = nightNow ? colScreenNight : colScreen;
  uint16_t ink = nightNow ? colInkNight : colInk;

  // top status bar
  M5.Display.fillRect(0, 0, w, topH, colBg);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(colMuted, colBg);
  M5.Display.setTextSize(1);
  const char* statusLabel = status == VACANT ? "VACANT" : (status == RUNNING ? "ON TRIP" : "STOPPED");
  M5.Display.drawString(statusLabel, 4, 5);

  M5.Display.setTextDatum(top_right);
  bool fix = gps.location.isValid();
  M5.Display.setTextColor(fix ? colGreen : colRed, colBg);
  M5.Display.drawString(fix ? "GPS FIX" : "NO FIX", w - 4, 5);

  // main LCD-style panel
  M5.Display.fillRoundRect(2, screenY, w - 4, screenH, 8, bg);

  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(ink, bg);
  M5.Display.setTextSize(1);
  M5.Display.drawString(currentRegion().name, 10, screenY + 6);

  char hms[10];
  formatHMS(elapsedSec, hms);
  M5.Display.setTextSize(2);
  M5.Display.drawString(hms, 10, screenY + 22);

  // fare, big, right aligned
  char fareBuf[16];
  sprintf(fareBuf, "%ld", computeFare());
  M5.Display.setTextDatum(top_right);
  M5.Display.setTextSize(4);
  M5.Display.drawString(fareBuf, w - 14, screenY + 14);
  M5.Display.setTextSize(1);
  M5.Display.drawString("FARE", w - 14, screenY + 4);

  // four mini cells
  int cellY = screenY + 50;
  int cellH = screenH - 50 - 6;
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
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(ink, bg);
    M5.Display.setTextSize(1);
    M5.Display.drawString(labels[i], cx, cellY);
    M5.Display.setTextSize(2);
    M5.Display.drawString(vals[i], cx, cellY + 14);
  }

  if (resetArmedAt != 0 && status == RUNNING) {
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(colAmber, bg);
    M5.Display.setTextSize(1);
    M5.Display.drawString("TAP F1 AGAIN TO RESET", 10, screenY + screenH - 12);
  }

  drawButtons();
}

void drawReceipt() {
  int w = M5.Display.width();
  int h = M5.Display.height();
  M5.Display.fillRect(20, 20, w - 40, h - 40, colWhite);
  M5.Display.drawRect(20, 20, w - 40, h - 40, colInk);

  long fare = computeFare();
  long night = computeNight(fare);
  long toll = tollCount * HIGHWAY_FEE;
  long total = fare + night + toll;

  int x = 34, y = 32, lh = 18;
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(colInk, colWhite);
  M5.Display.setTextSize(2);
  M5.Display.drawString("RECEIPT", x, y); y += lh + 6;
  M5.Display.setTextSize(1);
  M5.Display.drawString(currentRegion().name, x, y); y += lh;
  char hms[10]; formatHMS(elapsedSec, hms);
  char line[48];
  sprintf(line, "Trip time: %s", hms); M5.Display.drawString(line, x, y); y += lh;
  sprintf(line, "Distance: %.2f km", distanceKm); M5.Display.drawString(line, x, y); y += lh;
  sprintf(line, "Fare:   $%ld", fare); M5.Display.drawString(line, x, y); y += lh;
  sprintf(line, "Toll:   $%ld", toll); M5.Display.drawString(line, x, y); y += lh;
  sprintf(line, "Night:  $%ld", night); M5.Display.drawString(line, x, y); y += lh + 4;
  M5.Display.setTextSize(2);
  sprintf(line, "TOTAL: $%ld", total); M5.Display.drawString(line, x, y);

  M5.Display.setTextDatum(bottom_center);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(colMuted, colWhite);
  M5.Display.drawString("tap anywhere to close", w / 2, h - 26);
}

// ---------------- Touch ----------------
bool pointIn(const Btn& b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;

  if (showingReceipt) {
    showingReceipt = false;
    return;
  }

  if (pointIn(buttons[0], t.x, t.y)) { onVacant(); return; }
  if (pointIn(buttons[1], t.x, t.y)) { onStart(); return; }
  if (pointIn(buttons[2], t.x, t.y)) { onStop(); return; }
  if (pointIn(buttons[3], t.x, t.y)) { onHighway(); return; }
  if (pointIn(buttons[4], t.x, t.y)) { onReceipt(); return; }

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

  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  lastSecondMillis = millis();
  M5.Display.fillScreen(colBg);
  drawScreen();
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
  }
}
