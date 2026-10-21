/*
  NIKA - the UNO side (UNO + WiFi R3 board, like MILA)
  ---------------------------------------------------------------------------
  Made from MILA's sketch: the same tracks, modes (WASD / TANK / OBSTACLE), IR remote, NORA's IR
  fleet link, guard, Brainfuck talk and SCAN, and the same serial lines to her ESP8266. New:

    - a 16-LED RGB array (WS2812) instead of MILA's one RGB LED
    - a 128x32 SPI OLED (SSD1306) with what she's doing and thinking (SSD1306Ascii: no screen buffer
      in RAM, the UNO has 2 KB)
    - an infrared obstacle sensor (on/off: LOW = something in front, range set by its pot) next to the
      servo-mounted ultrasonic: both stop her
    - her mind is a synthesizer (THE SYNTH BRAIN below): she thinks in square waves

  PINS (a first plan: change the numbers here to match the wiring)
    D0/D1  ESP8266 serial (DIP 1 + 2)       D2/D3  ultrasonic TRIG / ECHO
    D4 IN1, D7 IN2, D5 ENA (right motor)    A2 IN3, A4 IN4, D6 ENB (left motor)
    A0     16 RGB array data                A1     servo (the ultrasonic's)
    A3     IR remote receiver               D12    IR obstacle sensor OUT
    D8     buzzer
    D9 / D10 / D11 / D13   OLED DC / CS / MOSI(SDA) / SCK(SCL); OLED RST to the board's RESET
    A5     synth: the digital pot's CS (MCP41010, on the same SPI wires)

  Why these: ENA and ENB need PWM, and on an UNO the Servo library takes Timer1 (no PWM on 9, 10) and
  tone() + the IR receiver take Timer2 (3, 11). Timer0's 5 and 6 are the only PWM pins left, so the
  RGB array (it needs no PWM) is on A0. D12 is SPI's input pin: free to read, not to drive.
*/
#include <Servo.h>
#include <IRremote.hpp>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include "SSD1306Ascii.h"
#include "SSD1306AsciiSpi.h"

// === PINS ===
const int trigPin = 2;
const int echoPin = 3;
const int irPin = A3;            // IR remote receiver
const int irObstaclePin = 12;    // IR obstacle sensor: LOW = something there

// Motor R
const int IN1 = 4;
const int IN2 = 7;
const int ENA = 5;

// Motor L
const int IN3 = A2;
const int IN4 = A4;
const int ENB = 6;

// 16-LED RGB array
const int ledPin = A0;
const int LED_COUNT = 16;
Adafruit_NeoPixel leds(LED_COUNT, ledPin, NEO_GRB + NEO_KHZ800);

// Servo
const int servoPin = A1;
Servo myServo;

// Buzzer
const int buzzerPin = 8;

// OLED (SPI) and the synth's digital pot, on the same SPI wires
const int oledDC = 9;
const int oledCS = 10;
const int synthCS = A5;
const bool OLED_FLIP = true;        // her screen is mounted upside down: turn the picture 180 degrees
SSD1306AsciiSpi oled;

unsigned long lastNoraHeardMs = 0;  // NORA's last IR talk / beacon (runLinkCommand): THE SYNTH BRAIN

// === TUNING ===
int turnTime = 550;                       // ms — about a 180; obstacle turns are a random 1/4 to all of this
int stopDist = 35;                        // cm — how close before stopping (obstacle mode)
int diagStopDist = 35;                    // cm — same, for the diagonal looks
int backupTime = 250;                     // ms — reverse this long before the scan so the tracks have room to pivot (0 = off)
int guardDist = 15;                       // cm — how close before force-stopping while driving forward in WASD/TANK
const unsigned long GUARD_POLL_MS = 150;  // how often to check the front sensor outside obstacle mode

// While driving in obstacle mode the sensor looks ahead and to both
// diagonals in turn. A wall met at an angle bounces the ping away from a
// straight-ahead sensor, so it reads as clear; the diagonal looks face
// the wall more squarely and catch it before a track corner does.
// 130 = left diagonal, 50 = right diagonal. Only the straight-ahead look
// is reported as DIST, so telemetry keeps meaning "front distance".
const int lookAngles[] = { 90, 130, 90, 50 };
const int NUM_LOOKS = 4;
const int LOOK_SETTLE_MS = 120;  // let the servo stop before pinging
int lookIndex = 0;

// === SPEED ===
// Single-button speed control (X on the dashboard keyboard, OK on the IR
// remote) cycles through these presets rather than needing separate +/-,
// since the remote only has the one spare button to give it.
const int speedPresets[] = { 100, 75, 50, 25 };
const int NUM_SPEED_PRESETS = 4;
int speedIndex = 0;
int speedPct = 100;

// NORA's LOCK over the fleet link (LINK_LOCK): the wheels stay still, whatever drives them
// (remote, link, obstacle mode), until her UNLOCK (LINK_UNLOCK). Unlocked at boot.
bool linkLocked = false;

uint8_t thoughtSpeedPct();   // THE SYNTH BRAIN, below
bool exploring();            // in OBSTACLE mode (her own drive)
int pwmSpeed() {
  if (linkLocked) return 0;   // every motor function goes through here
  int pct = speedPct;
  if (exploring()) pct = pct * thoughtSpeedPct() / 100;   // exploring by herself
  return (255 * pct) / 100;
}

void cycleSpeed() {
  speedIndex = (speedIndex + 1) % NUM_SPEED_PRESETS;
  speedPct = speedPresets[speedIndex];
  Serial.print("SPEED:");
  Serial.println(speedPct);
}

// Sets speed directly (dashboard slider), rather than stepping through
// the presets. Keeps speedIndex roughly in sync so a later IR/keyboard
// cycle continues from somewhere sane instead of jumping back to 100.
void setSpeedPct(int v) {
  v = constrain(v, 0, 100);
  speedPct = v;
  for (int i = 0; i < NUM_SPEED_PRESETS; i++) {
    if (speedPresets[i] == v) {
      speedIndex = i;
      break;
    }
  }
  Serial.print("SPEED:");
  Serial.println(speedPct);
}

// === IR REMOTE ===
// Codes read directly off this remote via the dashboard's "LAST IR" readout.
#define IR_FORWARD 0x74
#define IR_BACKWARD 0x75
#define IR_LEFT 0x34
#define IR_RIGHT 0x33
#define IR_OK 0x65

#define IR_MODE_WASD 0x0      // "1"
#define IR_MODE_TANK 0x1      // "2"
#define IR_MODE_OBSTACLE 0x2  // "3"

#define IR_RED 0x25     // tank: left track forward
#define IR_GREEN 0x26   // tank: left track backward
#define IR_YELLOW 0x27  // tank: right track forward
#define IR_BLUE 0x24    // tank: right track backward

// Toggles buzzer silent mode. Off until the real code is known: press the
// button with the Serial Monitor open, then uncomment this with its IR:0x.. value.
// #define IR_MUTE   0x00

const unsigned long IR_HOLD_MS = 350;  // no repeat frame within this long = WASD button released
unsigned long lastIRMs = 0;
uint8_t lastIRCmd = 0;
bool irDriving = false;  // true while a held WASD-mode IR key is driving the motors
unsigned long holdMs = IR_HOLD_MS;  // release timeout for the current drive (remote or link)

// === FLEET LINK (commands relayed by NORA) ===
// NORA's web page, Python controller and Bluetooth link can drive NIKA through
// NORA's IR transmitter: Samsung-format frames at NIKA_LINK_ADDRESS, with the
// same command codes the whole fleet link uses (IDA is 0x0DA1, WHIP 0x0DA3,
// KIDA-01 0x0DA4, MILA 0x0DA2). Drive commands are re-sent every 150 ms while held; NIKA
// stops once they've been quiet for LINK_HOLD_MS. Each command is translated
// to the matching remote button, so it behaves exactly like the remote.
#define NIKA_LINK_ADDRESS 0x0DA5
#define LINK_FORWARD   0x48
#define LINK_BACKWARD  0x49
#define LINK_LEFT      0x4A
#define LINK_RIGHT     0x4B
#define LINK_STOP      0x4C
#define LINK_OBSTACLE  0x4D
#define LINK_MANUAL    0x4E   // WASD mode
#define LINK_SPEED     0x4F   // cycle speed (the OK button)
#define LINK_UNLOCK    0x50   // NORA frees the wheels
#define LINK_LOCK      0x51   // NORA locks them: stop, and no driving until LINK_UNLOCK

// The wheel lock changes here, from NORA's IR link or the ESP8266 (/motorlockOn, /motorlockOff):
// locking stops her and leaves obstacle mode, and the ESP8266 hears LOCK:1 / LOCK:0 for /status.
void setLinkLocked(bool locked) {
  linkLocked = locked;
  if (locked) runIRCommand(IR_MODE_WASD);
  Serial.print("LOCK:");
  Serial.println(locked ? 1 : 0);
}
const unsigned long LINK_HOLD_MS = 500;

// Tank mode is button-toggled, not held: an IR receiver only ever decodes
// one button at a time, so "hold both tracks forward" isn't possible the
// way a two-stick controller would do it. Each color latches its track on
// or off instead, so pressing RED then YELLOW drives both tracks forward.
bool leftFwdOn = false, leftBwdOn = false, rightFwdOn = false, rightBwdOn = false;

// === LIGHTING ===
unsigned long lastStrobeTime = 0;
bool strobeState = false;

// === BUZZER ===
// Silent mode mutes everything except the startup tone, which always plays
// so you get audible confirmation the board booted even if muted last time.
bool buzzerSilent = false;

// On an Uno, tone() and IRremote share Timer2: a plain tone() leaves the
// timer reconfigured and the IR receiver dead until reset. So the receiver
// is paused for the length of the beep and restarted afterwards (as in
// IRremote's ReceiveDemo). Blocks for durMs.
void playTone(unsigned int freq, unsigned int durMs) {
  IrReceiver.stopTimer();
  tone(buzzerPin, freq, durMs);
  delay(durMs);
  IrReceiver.restartTimer();
}

void beep(unsigned int freq, unsigned int durMs) {
  if (buzzerSilent) return;
  // she has no speaker of her own to speak of: the WiFi chip passes every note on to whoever
  // listens (/sound: her web page's Listen, L on the controller), which plays it
  Serial.print("SND:");
  Serial.print(freq);
  Serial.print(",");
  Serial.println(durMs);
  playTone(freq, durMs);
}

// Getting close while driving forward: a short tick, faster the nearer the wall, like NORA's
// proximity tone and a car's parking sensor. Ticks (not one long tone) because the buzzer shares
// Timer2 with the IR receiver: beep() pauses it for 15 ms each time. From PROX_TICK_CM before
// `stopCm` (where she stops by herself) down to it; silent further away.
const long PROX_TICK_CM = 25;
void proximityTick(long d, long stopCm) {
  static unsigned long lastTick = 0;
  if (d <= 0 || d >= stopCm + PROX_TICK_CM) return;
  long near = constrain(d, stopCm, stopCm + PROX_TICK_CM);
  unsigned long gap = map(near, stopCm, stopCm + PROX_TICK_CM, 80, 600);
  if (millis() - lastTick < gap) return;
  lastTick = millis();
  beep(1400, 15);
}

void setBuzzerSilent(bool silent) {
  buzzerSilent = silent;
  Serial.print("BUZZER:");
  Serial.println(buzzerSilent ? 0 : 1);
}

// === SEQUENCES ===
const int cmySeq[3][3] = {
  { 0, 255, 255 },
  { 255, 0, 255 },
  { 255, 255, 0 }
};

// === MODE & STATE ===
enum DriveMode { MODE_WASD,
                 MODE_TANK,
                 MODE_OBSTACLE };
DriveMode driveMode = MODE_WASD;  // WASD is the boot default

enum RobotState { MOVING_FORWARD,
                  TURNING,
                  STOPPED };
RobotState robotState = STOPPED;

// True while the most recent manual command was a forward-driving one
// (WASD forward, or a tank track set forward) — used by checkManualGuard()
// to know when the front sensor should be watched outside obstacle mode.
// It only tracks the *last* command, not true per-track state, so a pivot
// like L_FWD then R_BWD will drop the guard even though the left track is
// still running forward — an accepted simplification given tank driving
// already has no held-state tracking for the web/pygame command path.
bool intentForward = false;
bool guardActive = false;  // true once the guard has force-stopped the robot

// === SENSOR CACHE (for ESP reporting) ===
long lastDist = 0;
long lastLeft = 0;
long lastRight = 0;

// =====================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(ENA, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(ENB, OUTPUT);

  pinMode(buzzerPin, OUTPUT);
  pinMode(irObstaclePin, INPUT);
  pinMode(synthCS, OUTPUT);
  digitalWrite(synthCS, HIGH);

  leds.begin();
  leds.setBrightness(60);   // 16 LEDs at full white would pull ~1 A from the board
  leds.show();

  SPI.begin();
  oled.begin(&Adafruit128x32, oledCS, oledDC);
  oled.displayRemap(OLED_FLIP);
  oled.setFont(System5x7);
  oled.clear();
  oled.println(F("NIKA"));
  oled.println(F("waking up..."));

  myServo.attach(servoPin);

  IrReceiver.begin(irPin, ENABLE_LED_FEEDBACK);

  // Servo sweep test
  myServo.write(30);
  delay(500);
  myServo.write(150);
  delay(500);
  myServo.write(90);
  delay(300);

  // Startup tone always plays, even in silent mode, so booting is audible.
  playTone(2000, 200);

  stopMotors();

  // Startup CMY sequence
  for (int i = 0; i < 3; i++) {
    setColor(cmySeq[i][0], cmySeq[i][1], cmySeq[i][2]);
    delay(333);
  }
  setColor(0, 0, 0);
  delay(200);

  Serial.println("DIST:0");
  Serial.println("LEFT:0");
  Serial.println("RIGHT:0");
  Serial.println("TURN:");
  Serial.print("SPEED:");
  Serial.println(speedPct);
  Serial.print("BUZZER:");
  Serial.println(buzzerSilent ? 0 : 1);
}

// =====================
void loop() {
  checkIR();
  checkSerial();
  brainLoop();
  oledLoop();

  if (driveMode != MODE_OBSTACLE) {
    checkManualGuard();
    updateLighting();
    return;
  }

  // === OBSTACLE AVOIDANCE ===
  int angle = lookAngles[lookIndex];
  lookIndex = (lookIndex + 1) % NUM_LOOKS;
  myServo.write(angle);
  delay(LOOK_SETTLE_MS);
  long d = getDistance();

  if (angle == 90) {
    lastDist = d;
    Serial.print("DIST:");
    Serial.println(d);
  } else {
    Serial.print(angle > 90 ? "DIAG_L:" : "DIAG_R:");
    Serial.println(d);
  }

  if (d > (angle == 90 ? stopDist : diagStopDist) && !irSeesSomething()) {
    if (angle == 90) proximityTick(d, stopDist);
    forward();
    robotState = MOVING_FORWARD;
    updateLighting();

  } else {
    stopMotors();
    lookIndex = 0;
    robotState = STOPPED;
    updateLighting();
    beep(400, 150);
    delay(100);

    if (backupTime > 0) {
      backward();
      delay(backupTime);
      stopMotors();
    }
    delay(200);

    // Scan left
    myServo.write(150);
    delay(500);
    lastLeft = getDistance();
    Serial.print("LEFT:");
    Serial.println(lastLeft);

    // Scan right
    myServo.write(30);
    delay(500);
    lastRight = getDistance();
    Serial.print("RIGHT:");
    Serial.println(lastRight);

    // Return to center
    myServo.write(90);
    delay(300);

    robotState = TURNING;
    updateLighting();

    unsigned long t = pickTurnTime(max(lastLeft, lastRight));
    if (lastLeft > lastRight) {
      Serial.println("TURN:LEFT");
      turnLeft();
    } else {
      Serial.println("TURN:RIGHT");
      turnRight();
    }
    delay(t);

    stopMotors();
    robotState = STOPPED;
    Serial.println("TURN:");
    delay(100);
  }

  updateLighting();
}

// Picks how long to pivot after the scan. turnTime is roughly a 180, so a
// fixed turnTime always spun her round. Instead the turn is sized by how
// open the chosen side is, with some randomness so she doesn't repeat the
// same move in a corner.
unsigned long pickTurnTime(long best) {
  static bool seeded = false;
  if (!seeded) { randomSeed(micros()); seeded = true; }   // first turn comes at a different moment every run
  if (best > 2 * stopDist) return random(turnTime / 4, turnTime / 2);        // wide open: ~45-90 deg
  if (best > stopDist)     return random(turnTime / 2, turnTime * 3 / 4);    // some room: ~90-135 deg
  return random(turnTime * 3 / 4, turnTime + 1);                             // boxed in: ~135-180 deg
}

// =====================
void checkIR() {
  if (IrReceiver.decode()) {
    if (IrReceiver.decodedIRData.protocol == SAMSUNG && IrReceiver.decodedIRData.address == NIKA_LINK_ADDRESS) {
      bool isRepeat = IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT;
      uint8_t c = IrReceiver.decodedIRData.command;
      lastIRMs = millis();
      Serial.print("LINK:0x");
      Serial.println(c, HEX);
      runLinkCommand(c, isRepeat);

    } else if (IrReceiver.decodedIRData.protocol != UNKNOWN) {
      bool isRepeat = IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT;
      if (!isRepeat) lastIRCmd = IrReceiver.decodedIRData.command;
      lastIRMs = millis();
      if (!isRepeat) holdMs = IR_HOLD_MS;

      Serial.print("IR:0x");
      Serial.println(lastIRCmd, HEX);

      if (!isRepeat) runIRCommand(lastIRCmd);
    }
    IrReceiver.resume();

  } else if (driveMode == MODE_WASD && irDriving && millis() - lastIRMs > holdMs) {
    // no repeat frame arrived in time — treat the WASD button as released
    stopMotors();
    robotState = STOPPED;
    irDriving = false;
    intentForward = false;
    updateLighting();
  }
}

// A command relayed by NORA over the fleet link, run as the matching remote
// button. Driving switches NIKA into WASD mode first, so one press from NORA
// is enough; repeated frames keep the drive alive until the link goes quiet.
void runLinkCommand(uint8_t c, bool isRepeat) {
  uint8_t ir = 0;
  switch (c) {
    case LINK_FORWARD:  ir = IR_FORWARD;  break;
    case LINK_BACKWARD: ir = IR_BACKWARD; break;
    case LINK_LEFT:     ir = IR_LEFT;     break;
    case LINK_RIGHT:    ir = IR_RIGHT;    break;
    case LINK_STOP:     runIRCommand(IR_MODE_WASD); return;   // stops, and leaves obstacle mode
    case LINK_OBSTACLE: if (!isRepeat) runIRCommand(IR_MODE_OBSTACLE); return;
    case LINK_MANUAL:   if (!isRepeat) runIRCommand(IR_MODE_WASD);     return;
    case LINK_SPEED:    if (!isRepeat) runIRCommand(IR_OK);            return;
    case LINK_UNLOCK:   setLinkLocked(false); return;
    case LINK_LOCK:     setLinkLocked(true);  return;   // stop, out of obstacle mode
    default:
      if (c >= 0x40 && c <= 0x47) lastNoraHeardMs = millis();   // NORA's talk / beacon: she's near
      return;
  }
  if (driveMode != MODE_WASD) runIRCommand(IR_MODE_WASD);
  holdMs = LINK_HOLD_MS;
  if (!irDriving || ir != lastIRCmd) {
    lastIRCmd = ir;
    runIRCommand(ir);
  }
}

// =====================
void resetManualState() {
  stopMotors();
  robotState = STOPPED;
  irDriving = false;
  leftFwdOn = leftBwdOn = rightFwdOn = rightBwdOn = false;
  intentForward = false;
  clearGuard();
}

// Clears the collision-guard warning; called on every new manual drive
// command so the dashboard's "GUARD ACTIVE" banner doesn't stick around
// after the driver has already reacted to it.
void clearGuard() {
  if (guardActive) {
    guardActive = false;
    Serial.println("GUARD:0");
  }
}

// Polls the front sensor while manually driving (WASD/TANK), so DIST stays
// fresh even while stopped or turning (telemetry logs are used to train a
// driving policy, and the human decides exactly then). It only force-stops
// if something gets within guardDist while intentForward is set (i.e. not
// while stopped or turning). Obstacle mode does its own sensing/scanning.
void checkManualGuard() {
  static unsigned long lastGuardPoll = 0;
  if (millis() - lastGuardPoll < GUARD_POLL_MS) return;
  lastGuardPoll = millis();

  lastDist = getDistance();
  Serial.print("DIST:");
  Serial.println(lastDist);
  if (intentForward) proximityTick(lastDist, guardDist);

  if (intentForward && ((lastDist > 0 && lastDist < guardDist) || irSeesSomething())) {
    stopMotors();
    robotState = STOPPED;
    intentForward = false;
    if (!guardActive) {
      guardActive = true;
      Serial.println("GUARD:1");
      beep(400, 150);
    }
  }
}

// Applies the four tank-track toggle latches to the motor pins.
void applyTankMotors() {
  if (leftFwdOn) leftMotorFwd();
  else if (leftBwdOn) leftMotorBwd();
  else leftMotorOff();

  if (rightFwdOn) rightMotorFwd();
  else if (rightBwdOn) rightMotorBwd();
  else rightMotorOff();

  robotState = (leftFwdOn || leftBwdOn || rightFwdOn || rightBwdOn) ? MOVING_FORWARD : STOPPED;
  intentForward = (leftFwdOn || rightFwdOn);
  clearGuard();
  updateLighting();
}

// =====================
void runIRCommand(uint8_t cmd) {
  switch (cmd) {
    case IR_MODE_WASD:
      driveMode = MODE_WASD;
      resetManualState();
      break;

    case IR_MODE_TANK:
      driveMode = MODE_TANK;
      resetManualState();
      break;

    case IR_MODE_OBSTACLE:
      driveMode = MODE_OBSTACLE;
      resetManualState();
      break;

    case IR_OK:
      cycleSpeed();
      break;

#ifdef IR_MUTE
    case IR_MUTE:
      setBuzzerSilent(!buzzerSilent);
      break;
#endif

    case IR_FORWARD:
      if (driveMode == MODE_WASD) {
        clearGuard();
        intentForward = true;
        forward();
        robotState = MOVING_FORWARD;
        irDriving = true;
      }
      break;

    case IR_BACKWARD:
      if (driveMode == MODE_WASD) {
        clearGuard();
        intentForward = false;
        backward();
        robotState = MOVING_FORWARD;
        irDriving = true;
      }
      break;

    case IR_LEFT:
      if (driveMode == MODE_WASD) {
        clearGuard();
        intentForward = false;
        turnLeft();
        robotState = TURNING;
        irDriving = true;
      }
      break;

    case IR_RIGHT:
      if (driveMode == MODE_WASD) {
        clearGuard();
        intentForward = false;
        turnRight();
        robotState = TURNING;
        irDriving = true;
      }
      break;

    case IR_RED:
      if (driveMode == MODE_TANK) {
        leftFwdOn = !leftFwdOn;
        leftBwdOn = false;
        applyTankMotors();
      }
      break;

    case IR_GREEN:
      if (driveMode == MODE_TANK) {
        leftBwdOn = !leftBwdOn;
        leftFwdOn = false;
        applyTankMotors();
      }
      break;

    case IR_YELLOW:
      if (driveMode == MODE_TANK) {
        rightFwdOn = !rightFwdOn;
        rightBwdOn = false;
        applyTankMotors();
      }
      break;

    case IR_BLUE:
      if (driveMode == MODE_TANK) {
        rightBwdOn = !rightBwdOn;
        rightFwdOn = false;
        applyTankMotors();
      }
      break;
  }
  updateLighting();
}

// ---- TALKING: the fleet's conversations, in Brainfuck ----
// "TALK:<u>" says utterance u (0-6 phrases, 7-13 replies; see talk_bf.h): a
// Brainfuck program that prints the words, beeped one tone per symbol, a
// little higher for NIKA. Played through beep(), so it pauses the IR receiver
// and respects MUTE; only while she's stopped outside OBSTACLE mode, since
// it blocks for the 1-4 s it takes.
#include "talk_bf.h"
const uint8_t TALK_VOICE_PCT = 125;   // a little higher than MILA's

void talk(int u) {
  if (robotState != STOPPED || driveMode == MODE_OBSTACLE) return;
  talkPlayBlocking(u, TALK_VOICE_PCT, beep);
}

// ---- SCAN: a fresh look on demand ----
// "SCAN" stops the tracks, sweeps the sensor left / ahead / right and prints
// "SCAN:<left>,<front>,<right>" in cm (999 = no echo). It only looks, never
// drives: whoever asked (an LLM driving her by stop-look-think-move) decides
// the next move. Obstacle mode is already sweeping and driving on its own, so
// there it answers "SCAN:BUSY". Blocks about 1.5 s.
const int SCAN_SETTLE_MS = 350;   // a 60 degree swing, then the servo stops shaking

long scanAt(int angle, int settleMs) {
  myServo.write(angle);
  delay(settleMs);
  return getDistance();
}

void scanAround() {
  if (driveMode == MODE_OBSTACLE) {
    Serial.println("SCAN:BUSY");
    return;
  }
  stopMotors();
  robotState = STOPPED;
  intentForward = false;
  updateLighting();

  lastLeft = scanAt(150, SCAN_SETTLE_MS + 150);   // from ahead, or from the right: the longest swing
  lastDist = scanAt(90, SCAN_SETTLE_MS);
  lastRight = scanAt(30, SCAN_SETTLE_MS);
  myServo.write(90);                               // back to looking ahead, as the guard expects

  Serial.print("LEFT:");
  Serial.println(lastLeft);
  Serial.print("DIST:");
  Serial.println(lastDist);
  Serial.print("RIGHT:");
  Serial.println(lastRight);
  Serial.print("SCAN:");
  Serial.print(lastLeft);
  Serial.print(",");
  Serial.print(lastDist);
  Serial.print(",");
  Serial.println(lastRight);
  delay(SCAN_SETTLE_MS);                           // settled ahead again before the guard pings
}

// =====================
void checkSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd == "WHO") {
    Serial.println("I am Nika");   // the fleet-wide handshake
  } else if (cmd.startsWith("TALK:")) {
    talk(cmd.substring(5).toInt());
  } else if (cmd == "SCAN") {
    scanAround();
  } else if (cmd == "OBSTACLE") {
    driveMode = MODE_OBSTACLE;
    resetManualState();

  } else if (cmd == "TANK") {
    driveMode = MODE_TANK;
    resetManualState();

  } else if (cmd == "WASD" || cmd == "MANUAL") {
    driveMode = MODE_WASD;
    resetManualState();

  } else if (cmd == "SPEEDCYCLE") {
    cycleSpeed();

  } else if (cmd.startsWith("SPEED:")) {
    setSpeedPct(cmd.substring(6).toInt());

  } else if (cmd == "MUTE") {
    setBuzzerSilent(true);

  } else if (cmd == "UNMUTE") {
    setBuzzerSilent(false);

  } else if (cmd == "LOCK") {       // the ESP8266's /motorlockOn
    setLinkLocked(true);

  } else if (cmd == "UNLOCK") {     // its /motorlockOff, after it checked her PIN
    setLinkLocked(false);

  } else if (driveMode == MODE_OBSTACLE) {
    return;

  } else if (cmd == "FORWARD") {
    clearGuard();
    intentForward = true;
    forward();
    robotState = MOVING_FORWARD;

  } else if (cmd == "BACKWARD") {
    clearGuard();
    intentForward = false;
    backward();
    robotState = MOVING_FORWARD;

  } else if (cmd == "LEFT") {
    clearGuard();
    intentForward = false;
    turnLeft();
    robotState = TURNING;

  } else if (cmd == "RIGHT") {
    clearGuard();
    intentForward = false;
    turnRight();
    robotState = TURNING;

  } else if (cmd == "STOP") {
    clearGuard();
    intentForward = false;
    stopMotors();
    robotState = STOPPED;

  } else if (cmd == "L_FWD") {
    clearGuard();
    intentForward = true;
    leftMotorFwd();
    robotState = MOVING_FORWARD;

  } else if (cmd == "L_BWD") {
    clearGuard();
    intentForward = false;
    leftMotorBwd();
    robotState = MOVING_FORWARD;

  } else if (cmd == "R_FWD") {
    clearGuard();
    intentForward = true;
    rightMotorFwd();
    robotState = MOVING_FORWARD;

  } else if (cmd == "R_BWD") {
    clearGuard();
    intentForward = false;
    rightMotorBwd();
    robotState = MOVING_FORWARD;
  }

  updateLighting();
}

// =====================
void updateLighting() {
  unsigned long now = millis();

  if (robotState == MOVING_FORWARD) {
    showThoughtColour();   // driving: the whole array is the colour of what she's thinking

  } else if (robotState == TURNING) {
    if (now - lastStrobeTime >= 100) {
      strobeState = !strobeState;
      lastStrobeTime = now;
      if (strobeState) setColor(255, 255, 255);
      else setColor(0, 0, 0);
    }

  } else {
    setColor(255, 0, 0);
  }
}

// =====================
// The whole array one colour. leds.show() stops interrupts for half a millisecond, which can spoil
// an IR frame arriving just then, so it only runs when the colour changes.
uint32_t shownColour = 0xFFFFFFFF;
void setColor(int r, int g, int b) {
  uint32_t c = leds.Color(r, g, b);
  if (c == shownColour) return;
  shownColour = c;
  leds.fill(c);
  leds.show();
}

// =====================
long getDistance() {
  long total = 0;
  for (int i = 0; i < 3; i++) {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    long duration = pulseIn(echoPin, HIGH, 38000);
    total += (duration == 0) ? 999 : duration * 0.0343 / 2;
    delay(10);
  }
  return total / 3;
}

// =====================
// Actual motor state, reported over serial as "MOTOR:<left>,<right>" (each -1 back, 0 off, +1 forward)
// whenever it changes. This is the one place that knows what the robot is really doing, whether the
// order came from the IR remote, the ESP (dashboard / keyboard / gamepad) or obstacle avoidance, so
// telemetry logs can be used to train a driving policy.
int motorL = 0, motorR = 0;

void reportMotors(int l, int r) {
  if (l == motorL && r == motorR) return;
  motorL = l;
  motorR = r;
  Serial.print("MOTOR:");
  Serial.print(l);
  Serial.print(",");
  Serial.println(r);
}

void stopMotors() {
  reportMotors(0, 0);
  digitalWrite(ENA, LOW);
  digitalWrite(ENB, LOW);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void forward() {
  reportMotors(1, 1);
  analogWrite(ENA, pwmSpeed());
  analogWrite(ENB, pwmSpeed());
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void backward() {
  reportMotors(-1, -1);
  analogWrite(ENA, pwmSpeed());
  analogWrite(ENB, pwmSpeed());
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void turnLeft() {
  reportMotors(-1, 1);
  analogWrite(ENA, pwmSpeed());
  analogWrite(ENB, pwmSpeed());
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void turnRight() {
  reportMotors(1, -1);
  analogWrite(ENA, pwmSpeed());
  analogWrite(ENB, pwmSpeed());
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void leftMotorFwd() {
  reportMotors(1, motorR);
  analogWrite(ENB, pwmSpeed());
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void leftMotorBwd() {
  reportMotors(-1, motorR);
  analogWrite(ENB, pwmSpeed());
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void leftMotorOff() {
  reportMotors(0, motorR);
  analogWrite(ENB, 0);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void rightMotorFwd() {
  reportMotors(motorL, 1);
  analogWrite(ENA, pwmSpeed());
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
}

void rightMotorBwd() {
  reportMotors(motorL, -1);
  analogWrite(ENA, pwmSpeed());
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
}

void rightMotorOff() {
  reportMotors(motorL, 0);
  analogWrite(ENA, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}

// =====================
// The IR obstacle sensor: LOW while something is within the range set by its pot.
bool irSeesSomething() {
  return digitalRead(irObstaclePin) == LOW;
}

// =====================
// THE SYNTH BRAIN: NIKA thinks in square waves
// ---------------------------------------------------------------------------
// Her drives are oscillators. Each has a frequency that follows what she senses, and the highest
// one is her thought:
//   CALM     196 Hz (G3), the hum that's always there
//   CURIOUS  rises the longer the way ahead stays open (220 -> 660 Hz over 30 s), and starts again
//            low after something gets close: wanting to explore
//   CAUTION  rises as something gets close: 220 Hz at 60 cm to 880 Hz at 5 cm, 880 when the IR
//            sensor sees something
//   SOCIAL   523 Hz (C5) when she hears NORA (her talk or "I'm here" beacon over the IR link),
//            falling back over 10 s: someone's near
// The thought is heard, seen and acted on:
//   - the synthesizer plays it: the digital pot (MCP41010) where the kit's frequency pot was, so
//     NIKA sets its pitch (an XR2206-style kit: higher resistance = lower note; with the square-wave
//     jumper it's a square wave). With no pot fitted the writes do nothing.
//   - the RGB array shows its colour while she drives, the OLED its name
//   - in OBSTACLE mode it sets how fast she explores: curious 100%, calm 80%, social 55% (and a
//     hello beep when it starts), caution 45%
// She reports it to the ESP8266 as THOUGHT:<name>,<Hz> when it changes.
enum Thought : uint8_t { CALM, CURIOUS, CAUTION, SOCIAL };
const char* const THOUGHT_NAMES[] = { "calm", "curious", "caution", "social" };
const uint8_t THOUGHT_RGB[][3] = { { 0, 160, 140 }, { 150, 60, 255 }, { 255, 90, 0 }, { 255, 40, 140 } };
const uint8_t THOUGHT_SPEED[] = { 80, 100, 45, 55 };
Thought thought = CALM;
int thoughtHz = 196;
unsigned long openSinceMs = 0;      // the way ahead has been open since (0 = it isn't)
const unsigned long BRAIN_MS = 100;

uint8_t thoughtSpeedPct() { return THOUGHT_SPEED[thought]; }
bool exploring() { return driveMode == MODE_OBSTACLE; }

void showThoughtColour() {
  setColor(THOUGHT_RGB[thought][0], THOUGHT_RGB[thought][1], THOUGHT_RGB[thought][2]);
}

// The synthesizer's pitch: the pot code for a frequency (code 255 = the lowest note). Rough, for an
// XR2206 kit (f ~ 1 / RC): tune SYNTH_K by ear so a thought's Hz sounds right.
const long SYNTH_K = 25500;
void synthPlay(int hz) {
  uint8_t code = (uint8_t)constrain(SYNTH_K / max(hz, 1), 10, 255);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(synthCS, LOW);
  SPI.transfer(0x11);   // MCP41010: write pot 0
  SPI.transfer(code);
  digitalWrite(synthCS, HIGH);
  SPI.endTransaction();
}

void brainLoop() {
  static unsigned long last = 0;
  if (millis() - last < BRAIN_MS) return;
  last = millis();

  bool ir = irSeesSomething();
  long d = lastDist;                       // cm; 999 = no echo
  bool near = ir || (d > 0 && d < 60);

  int caution = ir ? 880 : near ? map(constrain(d, 5, 60), 60, 5, 220, 880) : 110;
  if (near) openSinceMs = 0;
  else if (!openSinceMs) openSinceMs = millis();
  long openS = openSinceMs ? (millis() - openSinceMs) / 1000 : 0;
  int curious = openSinceMs ? map(min(openS, 30L), 0, 30, 220, 660) : 110;
  unsigned long sinceNora = lastNoraHeardMs ? millis() - lastNoraHeardMs : 999999UL;
  int social = sinceNora < 10000 ? map(sinceNora, 0, 10000, 523, 150) : 110;
  int calm = 196;

  int hz[4] = { calm, curious, caution, social };
  Thought t = CALM;
  for (uint8_t i = 1; i < 4; i++) if (hz[i] > hz[t]) t = (Thought)i;

  if (t != thought) {
    thought = t;
    if (thought == SOCIAL && driveMode == MODE_OBSTACLE) { beep(1200, 60); beep(1600, 60); }   // hi!
    if (robotState == MOVING_FORWARD) showThoughtColour();
  }
  if (abs(hz[t] - thoughtHz) >= 5 || t != thought) {
    thoughtHz = hz[t];
    synthPlay(thoughtHz);
    Serial.print("THOUGHT:");
    Serial.print(THOUGHT_NAMES[thought]);
    Serial.print(",");
    Serial.println(thoughtHz);
  }
  static int irSent = -1;
  if (ir != irSent) {   // the IR sensor, for her dashboard
    irSent = ir;
    Serial.print("IRD:");
    Serial.println(ir ? 1 : 0);
  }
}

// =====================
// The OLED: 4 lines of 21 characters, redrawn twice a second.
void oledLoop() {
  static unsigned long last = 0;
  if (millis() - last < 500) return;
  last = millis();
  const char* mode = driveMode == MODE_OBSTACLE ? "exploring" : driveMode == MODE_TANK ? "tank" : "wasd";
  oled.setCursor(0, 0);
  oled.print(F("NIKA  "));
  oled.print(mode);
  if (linkLocked) oled.print(F(" LOCK"));
  oled.clearToEOL();
  oled.setCursor(0, 1);
  oled.print(F("ahead "));
  if (lastDist > 0 && lastDist < 999) { oled.print(lastDist); oled.print(F(" cm")); } else oled.print(F("clear"));
  if (irSeesSomething()) oled.print(F("  IR!"));
  oled.clearToEOL();
  oled.setCursor(0, 2);
  oled.print(F("thinks "));
  oled.print(THOUGHT_NAMES[thought]);
  oled.clearToEOL();
  oled.setCursor(0, 3);
  oled.print(thoughtHz);
  oled.print(F(" Hz  speed "));
  oled.print(speedPct);
  oled.clearToEOL();
}
