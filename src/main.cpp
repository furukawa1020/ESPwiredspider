#include <Arduino.h>

namespace {
// IN1, IN2, IN3, IN4: keep this wiring order.
constexpr uint8_t MOTOR_PINS[4] = {13, 14, 16, 17};
constexpr uint32_t STEPS_PER_REV = 4096;  // Approximate; calibrate on hardware.
constexpr uint32_t STEP_INTERVAL_US = 2000;
constexpr uint8_t HALF_STEP_SEQUENCE[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

// Each motor owns its pins, phase and timing for later multi-motor use.
class HalfStepMotor {
 public:
  HalfStepMotor(const uint8_t (&pins)[4], uint32_t intervalUs)
      : intervalUs_(intervalUs) {
    for (uint8_t i = 0; i < 4; ++i) {
      pins_[i] = pins[i];
    }
  }

  void begin() {
    for (uint8_t pin : pins_) {
      digitalWrite(pin, LOW);
      pinMode(pin, OUTPUT);
    }
    stop();
  }

  void move(int8_t direction, uint32_t steps) {
    if (steps == 0) {
      stop();
      return;
    }
    remainingSteps_ = steps;
    start(direction, Mode::Finite);
  }

  void run(int8_t direction) {
    remainingSteps_ = 0;
    start(direction, Mode::Continuous);
  }

  void stop() {
    mode_ = Mode::Stopped;
    remainingSteps_ = 0;
    for (uint8_t pin : pins_) {
      digitalWrite(pin, LOW);
    }
  }

  // Call frequently. Returns true once a finite move has finished.
  bool update(uint32_t nowUs) {
    if (mode_ == Mode::Stopped ||
        static_cast<uint32_t>(nowUs - lastStepUs_) < intervalUs_) {
      return false;
    }
    // Hold the last phase for a full interval before releasing the coils.
    if (mode_ == Mode::Finite && remainingSteps_ == 0) {
      stop();
      return true;
    }

    // Do not emit a burst of catch-up steps if the loop was delayed.
    lastStepUs_ = nowUs;
    phase_ = static_cast<uint8_t>((phase_ + (direction_ > 0 ? 1 : 7)) % 8);
    for (uint8_t i = 0; i < 4; ++i) {
      digitalWrite(pins_[i], HALF_STEP_SEQUENCE[phase_][i] ? HIGH : LOW);
    }
    if (mode_ == Mode::Finite) {
      --remainingSteps_;
    }
    return false;
  }

 private:
  enum class Mode { Stopped, Finite, Continuous };

  void start(int8_t direction, Mode mode) {
    direction_ = direction > 0 ? 1 : -1;
    mode_ = mode;
    lastStepUs_ = micros();
  }

  uint8_t pins_[4];
  const uint32_t intervalUs_;
  uint8_t phase_ = 0;
  int8_t direction_ = 1;
  Mode mode_ = Mode::Stopped;
  uint32_t remainingSteps_ = 0;
  uint32_t lastStepUs_ = 0;
};

HalfStepMotor motor(MOTOR_PINS, STEP_INTERVAL_US);

void printHelp() {
  Serial.println("=== 28BYJ-48 Motor Test ===");
  Serial.println("f : forward ~1 revolution (4096 half-steps)");
  Serial.println("b : backward ~1 revolution (4096 half-steps)");
  Serial.println("r : continuous forward");
  Serial.println("l : continuous backward");
  Serial.println("s : stop / coils off (also during a finite move)");
  Serial.println("New motion commands replace the current move.");
}

void handleSerial() {
  // Bound input handling so each motor can still be serviced under load.
  for (uint8_t count = 0; count < 16 && Serial.available() > 0; ++count) {
    const char command = static_cast<char>(Serial.read());
    switch (command) {
      case 'f':
      case 'F':
        motor.move(1, STEPS_PER_REV);
        Serial.println("Forward: ~1 revolution");
        break;
      case 'b':
      case 'B':
        motor.move(-1, STEPS_PER_REV);
        Serial.println("Backward: ~1 revolution");
        break;
      case 'r':
      case 'R':
        motor.run(1);
        Serial.println("Continuous forward");
        break;
      case 'l':
      case 'L':
        motor.run(-1);
        Serial.println("Continuous backward");
        break;
      case 's':
      case 'S':
        motor.stop();
        Serial.println("STOP: coils off");
        break;
      case '\n':
      case '\r':
      case ' ':
      case '\t':
        break;
      default:
        printHelp();
        break;
    }
  }
}
}  // namespace

void setup() {
  motor.begin();
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("ESP32 + ULN2003 + 28BYJ-48 ready. Coils off.");
  printHelp();
}

void loop() {
  handleSerial();
  if (motor.update(micros())) {
    Serial.println("Done: coils off");
  }
}
