#pragma once

#include <cstdint>

class XuesuConfirmInput {
 public:
  enum class Mode : uint8_t { Disabled, Double, Triple };
  enum class Action : uint8_t { None, Single, Double, Triple };
  static constexpr uint32_t clickGapMs = 300;

  Action update(const uint32_t now, const bool clicked, const bool held, const bool interrupted, const Mode mode) {
    if (mode == Mode::Disabled) {
      reset();
      return Action::None;
    }
    if (interrupted) {
      reset();
      return clicked ? Action::Single : Action::None;
    }

    if (pending != Action::None) {
      if (!nextContact && now - clickedAt > clickGapMs) {
        const auto action = pending;
        pending = clicked ? Action::Single : Action::None;
        clickedAt = now;
        return action;
      }
      if (!clicked) {
        nextContact = held;
        return Action::None;
      }
      if (pending == Action::Double) {
        reset();
        return Action::Triple;
      }
      if (mode == Mode::Double) {
        reset();
        return Action::Double;
      }
      pending = Action::Double;
      clickedAt = now;
      nextContact = false;
    } else if (clicked) {
      pending = Action::Single;
      clickedAt = now;
    }
    return Action::None;
  }

  void reset() {
    pending = Action::None;
    nextContact = false;
  }

 private:
  uint32_t clickedAt = 0;
  Action pending = Action::None;
  bool nextContact = false;
};
