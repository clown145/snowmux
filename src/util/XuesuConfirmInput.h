#pragma once

#include <cstdint>

class XuesuConfirmInput {
 public:
  enum class Action : uint8_t { None, Single, Double };
  static constexpr uint32_t doubleClickMs = 300;

  Action update(const uint32_t now, const bool clicked, const bool held, const bool interrupted) {
    if (interrupted) {
      reset();
      return clicked ? Action::Single : Action::None;
    }

    if (pending) {
      if (!secondContact && now - clickedAt > doubleClickMs) {
        pending = clicked;
        clickedAt = now;
        return Action::Single;
      }
      if (clicked) {
        reset();
        return Action::Double;
      }
      secondContact |= held;
    } else if (clicked) {
      pending = true;
      clickedAt = now;
    }
    return Action::None;
  }

  void reset() {
    pending = false;
    secondContact = false;
  }

 private:
  uint32_t clickedAt = 0;
  bool pending = false;
  bool secondContact = false;
};
