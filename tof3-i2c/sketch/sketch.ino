#include <Arduino_RouterBridge.h>
#include <Wire.h>
#include <vl53l8cx_api.h>

// Sensors 1-2 share Wire (SDA D20 / SCL D21). Sensor 3 is on its own bus,
// Wire2 (SDA A4 / SCL A5, i2c3 in the board's devicetree) — this halves how
// many boards' onboard pull-ups fight each other on the main bus, and a
// wiring fault on sensor 3 can no longer affect 1-2. Each LPn still needs
// its own GPIO regardless of which bus it's on.
constexpr uint8_t SENSOR_COUNT = 3;
constexpr uint8_t LPn_PINS[SENSOR_COUNT] = {2, 3, 4};
constexpr uint8_t SENSOR_ADDRESSES[SENSOR_COUNT] = {0x30, 0x31, 0x32};
constexpr uint8_t DEFAULT_ADDRESS = 0x29;
TwoWire *const SENSOR_BUS[SENSOR_COUNT] = {&Wire, &Wire, &Wire2};

// Timing. VL53L8CX datasheet t_BOOT after LPn goes high is ~1.2 ms typ / 2 ms max,
// so LPN_SETTLE_MS still has ~5x margin. If the bus is flaky (see README), raise
// these back up before assuming the timing is the problem.
constexpr uint16_t BOOT_SETTLE_MS = 500;    // was 2000: let all 3 sensors power up once
constexpr uint16_t LPN_SETTLE_MS = 10;      // was 100: wait after LPn HIGH, per attempt
constexpr uint16_t RETRY_INTERVAL_MS = 3000; // was 10000: how soon a failed sensor retries

struct SensorState {
  VL53L8CX_Configuration device = {};
  TwoWire *bus = nullptr;
  bool running = false;
  String phase = "waiting";
  String detail = "Starting";
  int errorCode = 0;
  uint32_t frames = 0;
  uint32_t lastFrame = 0;
  uint32_t lastAttempt = 0;
  uint32_t lastStatus = 0;
};

SensorState sensors[SENSOR_COUNT];
VL53L8CX_ResultsData result = {};

// handle is always a SensorState* (see connectSensor) so each sensor's I2C
// traffic goes out on its own bus (sensor.bus) instead of a hardcoded Wire.
uint8_t writeSensor(void *handle, uint16_t reg, uint8_t *data, uint32_t size) {
  auto *sensor = static_cast<SensorState *>(handle);
  TwoWire &wire = *sensor->bus;
  uint8_t address = sensor->device.platform.address >> 1;
  for (uint32_t offset = 0; offset < size;) {
    size_t count = min((uint32_t)128, size - offset);
    uint16_t registerAddress = reg + offset;
    wire.beginTransmission(address);
    wire.write((uint8_t)(registerAddress >> 8));
    wire.write((uint8_t)registerAddress);
    if (wire.write(data + offset, count) != count) return 1;
    if (wire.endTransmission() != 0) return 1;
    offset += count;
  }
  return 0;
}

uint8_t readSensor(void *handle, uint16_t reg, uint8_t *data, uint32_t size) {
  auto *sensor = static_cast<SensorState *>(handle);
  TwoWire &wire = *sensor->bus;
  uint8_t address = sensor->device.platform.address >> 1;
  for (uint32_t offset = 0; offset < size;) {
    size_t count = min((uint32_t)128, size - offset);
    uint16_t registerAddress = reg + offset;
    wire.beginTransmission(address);
    wire.write((uint8_t)(registerAddress >> 8));
    wire.write((uint8_t)registerAddress);
    if (wire.endTransmission(false) != 0) return 1;
    if (wire.requestFrom(address, count) != count) return 1;
    for (size_t n = 0; n < count; ++n) data[offset + n] = wire.read();
    offset += count;
  }
  return 0;
}

uint8_t waitSensor(void *, uint32_t ms) { delay(ms); return 0; }

void report(uint8_t index) {
  SensorState &sensor = sensors[index];
  Bridge.notify("tof3_status", (int)index, sensor.phase, sensor.detail, sensor.errorCode);
  Serial.println("ToF " + String(index + 1) + " " + sensor.phase + ": " + sensor.detail +
                 " (code " + String(sensor.errorCode) + ")");
  sensor.lastStatus = millis();
}

void fail(uint8_t index, const String &message, int code) {
  SensorState &sensor = sensors[index];
  sensor.running = false;
  digitalWrite(LPn_PINS[index], LOW);
  sensor.phase = "error";
  sensor.detail = message;
  sensor.errorCode = code;
  sensor.lastAttempt = millis();
  report(index);
}

void connectSensor(uint8_t index) {
  SensorState &sensor = sensors[index];
  sensor.lastAttempt = millis();
  sensor.running = false;
  sensor.bus = SENSOR_BUS[index];
  sensor.device.platform.address = DEFAULT_ADDRESS << 1;
  sensor.device.platform.Write = writeSensor;
  sensor.device.platform.Read = readSensor;
  sensor.device.platform.Wait = waitSensor;
  sensor.device.platform.handle = &sensor;

  sensor.phase = "checking";
  sensor.detail = "LPn D" + String(LPn_PINS[index]) + ", default I2C 0x29";
  sensor.errorCode = 0;
  report(index);
  digitalWrite(LPn_PINS[index], HIGH);
  delay(LPN_SETTLE_MS);

  uint8_t bank = 0, identity[2] = {};
  bool alreadyAssigned = false;
  if (writeSensor(&sensor, 0x7fff, &bank, 1) ||
      readSensor(&sensor, 0, identity, 2)) {
    // LPn isolates I2C but does not reset the chip or its assigned address.
    sensor.device.platform.address = SENSOR_ADDRESSES[index] << 1;
    if (writeSensor(&sensor, 0x7fff, &bank, 1) ||
        readSensor(&sensor, 0, identity, 2)) {
      fail(index, "No sensor at default/assigned address; check LPn, power, SDA/SCL", 1);
      return;
    }
    alreadyAssigned = true;
  }
  bank = 2;
  if (writeSensor(&sensor, 0x7fff, &bank, 1)) {
    fail(index, "Could not restore register bank", 1);
    return;
  }
  if (identity[0] != 0xf0 || identity[1] != 0x0c) {
    fail(index, "Unexpected sensor ID: 0x" + String(identity[0], HEX) +
                "/0x" + String(identity[1], HEX), 2);
    return;
  }

  uint8_t code = 0;
  if (!alreadyAssigned) {
    code = vl53l8cx_set_i2c_address(&sensor.device, SENSOR_ADDRESSES[index] << 1);
    if (code) { fail(index, "Address assignment failed", code); return; }
    bank = 0;
    if (writeSensor(&sensor, 0x7fff, &bank, 1) ||
        readSensor(&sensor, 0, identity, 2) ||
        identity[0] != 0xf0 || identity[1] != 0x0c) {
      fail(index, "Sensor did not respond at assigned address", 4);
      return;
    }
    bank = 2;
    if (writeSensor(&sensor, 0x7fff, &bank, 1)) {
      fail(index, "Could not restore assigned sensor bank", 1);
      return;
    }
  }

  sensor.phase = "initializing";
  sensor.detail = "Loading firmware at 0x" + String(SENSOR_ADDRESSES[index], HEX);
  report(index);
  code = vl53l8cx_init(&sensor.device);
  if (code) { fail(index, "Firmware initialization failed", code); return; }
  code = vl53l8cx_set_resolution(&sensor.device, VL53L8CX_RESOLUTION_8X8);
  if (code) { fail(index, "8x8 configuration failed", code); return; }
  code = vl53l8cx_set_ranging_frequency_hz(&sensor.device, 5);
  if (code) { fail(index, "5 Hz configuration failed", code); return; }
  code = vl53l8cx_start_ranging(&sensor.device);
  if (code) { fail(index, "Start ranging failed", code); return; }

  sensor.running = true;
  sensor.phase = "measuring";
  sensor.detail = "VL53L8CX at 0x" + String(SENSOR_ADDRESSES[index], HEX) +
                  ": 8x8, 5 Hz";
  sensor.lastFrame = millis();
  report(index);
}

void sendFrame(uint8_t index) {
  SensorState &sensor = sensors[index];
  String payload;
  payload.reserve(900);
  payload = "{\"seq\":" + String(++sensor.frames) + ",\"distance_mm\":[";
  for (int i = 0; i < 64; ++i) {
    if (i) payload += ',';
    int n = i * VL53L8CX_NB_TARGET_PER_ZONE;
    int status = result.target_status[n];
    bool valid = result.nb_target_detected[i] > 0 &&
                 (status == 5 || status == 9) && result.distance_mm[n] >= 0;
    payload += valid ? String(result.distance_mm[n]) : String("null");
  }
  payload += "],\"target_status\":[";
  for (int i = 0; i < 64; ++i) {
    if (i) payload += ',';
    payload += String(result.target_status[i * VL53L8CX_NB_TARGET_PER_ZONE]);
  }
  payload += "]}";
  Bridge.notify("tof3_frame", (int)index, payload);
}

void setup() {
  Serial.begin(115200);
  // Put all sensors in shutdown before addressing any one of them.
  for (uint8_t i = 0; i < SENSOR_COUNT; ++i) {
    digitalWrite(LPn_PINS[i], LOW);
    pinMode(LPn_PINS[i], OUTPUT);
  }
  Bridge.begin();
  Wire.begin();          // sensors 1-2 (D20/D21) — still share a bus, still share doubled-up
  Wire.setClock(100000);  // pull-ups (~509 ohm combined); reverted from 400000, see tof3-i2c-notes.md §1.4
  Wire2.begin();         // sensor 3, its own bus (A4/A5) — clean since the split, keep at 400 kHz
  Wire2.setClock(400000);
  delay(BOOT_SETTLE_MS);
  for (uint8_t i = 0; i < SENSOR_COUNT; ++i) connectSensor(i);
}

void loop() {
  for (uint8_t i = 0; i < SENSOR_COUNT; ++i) {
    SensorState &sensor = sensors[i];
    if (millis() - sensor.lastStatus > 2000) report(i);
    if (!sensor.running) {
      if (millis() - sensor.lastAttempt > RETRY_INTERVAL_MS) connectSensor(i);
      continue;
    }
    uint8_t ready = 0;
    uint8_t code = vl53l8cx_check_data_ready(&sensor.device, &ready);
    if (code) { fail(i, "Data-ready read failed", code); continue; }
    if (!ready) {
      if (millis() - sensor.lastFrame > 5000) fail(i, "No measurement for 5 seconds", 3);
      continue;
    }
    code = vl53l8cx_get_ranging_data(&sensor.device, &result);
    if (code) { fail(i, "Distance read failed", code); continue; }
    sensor.lastFrame = millis();
    sendFrame(i);
  }
  delay(10);
}
