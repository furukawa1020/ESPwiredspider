#include "Arduino.h"
#include <cassert>
#include <iostream>

uint32_t clockUs = 0;
int levels[40] = {};
std::vector<int> writes;
FakeSerial Serial;

#include "../../src/main.cpp"

static void command(char value) {
  Serial.input.push_back(value);
  loop();
}

static unsigned mask() {
  return levels[13] | (levels[14] << 1) | (levels[16] << 2) | (levels[17] << 3);
}

static void tick(uint32_t elapsed = 2000) {
  clockUs += elapsed;
  loop();
}

int main() {
  setup();
  assert(Serial.baud == 115200 && mask() == 0);
  for (int pin : writes) {
    assert(pin == 13 || pin == 14 || pin == 16 || pin == 17);
  }

  // Verify physical output patterns, count and final dwell in both directions.
  const unsigned forward[] = {3, 2, 6, 4, 12, 8, 9, 1};
  const unsigned backward[] = {9, 8, 12, 4, 6, 2, 3, 1};
  for (char direction : {'f', 'b'}) {
    command(direction);
    writes.clear();
    tick(1999);
    assert(writes.empty());
    tick(1);
    assert(mask() == (direction == 'f' ? forward[0] : backward[0]));
    for (unsigned step = 1; step < 4096; ++step) {
      tick();
      assert(mask() == (direction == 'f' ? forward[step % 8] : backward[step % 8]));
    }
    assert(writes.size() == 4096 * 4 && mask() != 0);
    tick(1999);
    assert(mask() != 0);
    tick(1);
    assert(mask() == 0);
    const auto count = writes.size();
    tick();
    assert(writes.size() == count);
  }

  // Stop interrupts finite and continuous commands before their next step.
  for (char action : {'F', 'B', 'r', 'l'}) {
    command(action);
    tick();
    assert(mask() != 0);
    command('S');
    assert(mask() == 0);
    writes.clear();
    tick(10000000);
    assert(writes.empty());
  }

  command('R');
  for (unsigned step = 0; step < 4100; ++step) tick();
  assert(mask() != 0);
  command('b');  // Finite move replaces continuous mode.
  for (unsigned step = 0; step < 4097; ++step) tick();
  assert(mask() == 0);

  // Unsigned elapsed time survives micros() wrap and never catches up in bursts.
  clockUs = UINT32_MAX - 999;
  command('L');
  writes.clear();
  tick(1999);
  assert(writes.empty());
  tick(1);
  assert(writes.size() == 4);
  tick(100000);
  assert(writes.size() == 8);
  loop();
  assert(writes.size() == 8);
  command('s');
  command('\r');
  command('\n');
  command('?');
  assert(mask() == 0);
  assert(Serial.output.find("Done: coils off") != std::string::npos);
  std::cout << "PASS: phase order, step count, dwell, stop, replacement, timer wrap\n";
}
