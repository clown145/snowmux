#pragma once

#include <BleRemoteProfiles.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Binding EACH button of a BLE remote. A button is known by the fingerprint of its
// RAW HID frame (report id, byte index, value; see freeink::RawButtonEvent). The
// decoded usage is too lossy for that: the decoder drops the second byte of the
// three-button remote and cuts 16-bit usages to 8 bits. Each remote (by BLE address) has one table of at
// most 9 slots, saved in settings.json and applied again on reconnect.
//
// Binding policy is shared by the runtime, settings screen and tests. Device-specific
// report profiles come from the SDK; reader actions and saved overrides stay here.
namespace bleturner {

// One wait for a button lasts at most 15 seconds, as the screen tells the user.
inline constexpr uint32_t kWaitMs = 15000;
// Held this long or more is a "hold". A host whose own buttons skip chapters on a
// hold should use the same number and pin it with a static_assert.
inline constexpr uint32_t kHoldMs = 700;
// Maximum release-to-next-press gap in a multi-click gesture.
inline constexpr uint32_t kMultiClickMs = 350;
enum class Gesture : uint8_t { Tap, DoubleTap, TripleTap, Hold };

// Stored by value in settings.json (bits 28-31 of a slot): new actions go last, and the
// numbers never change (a test pins them). The four moves are the remote's own;
// ReaderMenu and SaveQuote are handed to the host, which runs its own action of that name.
enum class Action : uint8_t {
  None,
  NextPage,
  PrevPage,
  NextChapter,
  PrevChapter,
  ReaderMenu,
  SaveQuote,
  Confirm,
  Back,
  Up,
  Down,
  Left,
  Right,
  Sleep
};

// One slot: bits 0-23 the button code (value | byte index << 8 | report id << 16),
// bit 24 "hold", bit 25 "any report id" (built-in defaults only, since that remote's
// report id has not been measured), bit 26 "double", bit 27 "triple", bits 28-31 the action.
// Tap/hold encodings stay unchanged; only one gesture bit may be set.
using Binding = uint32_t;
inline constexpr uint32_t kCodeMask = 0x00FFFFFFu;
inline constexpr uint32_t kHoldBit = 1u << 24;
inline constexpr uint32_t kAnyReportBit = 1u << 25;
inline constexpr uint32_t kDoubleBit = 1u << 26;
inline constexpr uint32_t kTripleBit = 1u << 27;
inline constexpr uint32_t kGestureMask = kHoldBit | kDoubleBit | kTripleBit;
inline constexpr uint8_t kActionShift = 28;

inline constexpr uint8_t kMaxBindings = 9;  // All nine actions offered by the mapping screen.
inline constexpr uint8_t kMaxRemotes = 4;   // = BleKeyboardHost::kMaxBonds

struct RemoteTable {
  char addr[18];
  uint8_t count;
  Binding bindings[kMaxBindings];
};

constexpr uint32_t gestureBits(const Gesture gesture) {
  switch (gesture) {
    case Gesture::Tap:
      return 0;
    case Gesture::DoubleTap:
      return kDoubleBit;
    case Gesture::TripleTap:
      return kTripleBit;
    case Gesture::Hold:
      return kHoldBit;
  }
  return kGestureMask;  // Invalid enum values cannot become valid saved bindings.
}
constexpr Gesture gestureOf(const Binding b) {
  switch (b & kGestureMask) {
    case kDoubleBit:
      return Gesture::DoubleTap;
    case kTripleBit:
      return Gesture::TripleTap;
    case kHoldBit:
      return Gesture::Hold;
    default:
      return Gesture::Tap;  // Callers only pass validated slots.
  }
}
constexpr Binding makeBinding(const uint32_t code, const Gesture gesture, const Action action) {
  return (code & kCodeMask) | gestureBits(gesture) | static_cast<uint32_t>(action) << kActionShift;
}
// Keep the existing tap/hold callers and their persisted numbers compatible.
constexpr Binding makeBinding(const uint32_t code, const bool hold, const Action action) {
  return makeBinding(code, hold ? Gesture::Hold : Gesture::Tap, action);
}
constexpr Action actionOf(const Binding b) { return static_cast<Action>(b >> kActionShift); }

// A slot read from the card is kept only when every bit means something: an action
// in range, a non-zero value, no unknown bit. Garbage is dropped rather than turned
// into a button that does something odd.
constexpr bool valid(const Binding b) {
  const uint32_t gesture = b & kGestureMask;
  return (gesture == 0 || gesture == kHoldBit || gesture == kDoubleBit || gesture == kTripleBit) && (b & 0xFFu) != 0 &&
         actionOf(b) != Action::None && static_cast<uint8_t>(actionOf(b)) <= static_cast<uint8_t>(Action::Sleep);
}

constexpr bool matches(const Binding b, const uint32_t code, const Gesture gesture) {
  const uint32_t mask = ((b & kAnyReportBit) ? 0x0000FFFFu : kCodeMask) | kGestureMask;
  return ((b ^ ((code & kCodeMask) | gestureBits(gesture))) & mask) == 0;
}
constexpr bool matches(const Binding b, const uint32_t code, const bool hold) {
  return matches(b, code, hold ? Gesture::Hold : Gesture::Tap);
}

// Lookup: at most 9 compares over 36 contiguous bytes, no allocation.
inline Action lookup(const RemoteTable& t, const uint32_t code, const Gesture gesture) {
  for (uint8_t i = 0; i < t.count; ++i) {
    if (matches(t.bindings[i], code, gesture)) return actionOf(t.bindings[i]);
  }
  return Action::None;
}
inline Action lookup(const RemoteTable& t, const uint32_t code, const bool hold) {
  return lookup(t, code, hold ? Gesture::Hold : Gesture::Tap);
}

inline bool find(const RemoteTable& t, const Action action, Binding& out) {
  for (uint8_t i = 0; i < t.count; ++i) {
    if (actionOf(t.bindings[i]) == action) {
      out = t.bindings[i];
      return true;
    }
  }
  return false;
}

inline const char* actionName(const Action action) {
  static const char* const kNames[] = {"none",        "next",       "prev",    "next_chapter", "prev_chapter",
                                       "reader_menu", "save_quote", "confirm", "back",         "up",
                                       "down",        "left",       "right",   "sleep"};
  return kNames[static_cast<uint8_t>(action) <= static_cast<uint8_t>(Action::Sleep) ? static_cast<uint8_t>(action) : 0];
}

// "3:1=02" (report id 3, byte 1, value 0x02); "1=02" for an any-report default slot;
// "0x43" for a button known only by the key the decoder read (byte index 0xFF).
inline void formatCode(char* out, const size_t n, const Binding b) {
  if (((b >> 8) & 0xFF) == 0xFF) {
    snprintf(out, n, "0x%02X", static_cast<unsigned>(b & 0xFF));
  } else if (b & kAnyReportBit) {
    snprintf(out, n, "%u=%02X", static_cast<unsigned>((b >> 8) & 0xFF), static_cast<unsigned>(b & 0xFF));
  } else {
    snprintf(out, n, "%u:%u=%02X", static_cast<unsigned>((b >> 16) & 0xFF), static_cast<unsigned>((b >> 8) & 0xFF),
             static_cast<unsigned>(b & 0xFF));
  }
}

// Translate SDK logical keys into reader actions without changing saved raw codes.
inline const RemoteTable* defaultTableFor(const char* name) {
  static constexpr auto tables = [] {
    std::array<RemoteTable, std::size(freeink::kRemoteButtonProfiles)> result{};
    // RemoteKey order: Right, Left, PageDown, PageUp.
    constexpr Action actions[] = {Action::NextPage, Action::PrevPage, Action::NextChapter, Action::PrevChapter};
    static_assert(std::size(actions) == static_cast<size_t>(freeink::RemoteKey::Count));
    static_assert(std::size(freeink::kRemoteButtonProfiles[0].buttons) <= kMaxBindings);
    for (size_t i = 0; i < result.size(); ++i) {
      const auto& profile = freeink::kRemoteButtonProfiles[i];
      result[i].count = profile.count;
      for (uint8_t j = 0; j < profile.count; ++j) {
        const auto& button = profile.buttons[j];
        result[i].bindings[j] = makeBinding(button.code, false, actions[static_cast<uint8_t>(button.key)]) |
                                (button.anyReport ? kAnyReportBit : 0u);
      }
    }
    return result;
  }();
  const int index = freeink::remoteButtonProfileIndex(name);
  return index < 0 ? nullptr : &tables[index];
}

// Table of the connected remote: the saved one for its address, else the default for
// its name, else none (nullptr = the old usage path, exactly as today). Writes nothing.
inline const RemoteTable* tableFor(const RemoteTable* tables, const uint8_t count, const char* addr, const char* name) {
  if (addr == nullptr || addr[0] == '\0') return nullptr;
  for (uint8_t i = 0; i < count; ++i) {
    if (strncmp(tables[i].addr, addr, sizeof(tables[i].addr)) == 0) return &tables[i];
  }
  return defaultTableFor(name);
}

// Only a table with at least one slot changes the path. An empty table (cleared by
// the user) keeps its address so the default does not come back, and takes the old path.
inline bool routes(const RemoteTable* t) { return t != nullptr && t->count > 0; }

// Editable table of a remote: the saved one, or a new one copied from its default.
// nullptr when kMaxRemotes remotes already have one.
inline RemoteTable* editableTable(RemoteTable* tables, uint8_t& count, const char* addr, const char* name) {
  if (addr == nullptr || addr[0] == '\0') return nullptr;
  for (uint8_t i = 0; i < count; ++i) {
    if (strncmp(tables[i].addr, addr, sizeof(tables[i].addr)) == 0) return &tables[i];
  }
  if (count >= kMaxRemotes) return nullptr;
  RemoteTable& t = tables[count++];
  const RemoteTable* seed = defaultTableFor(name);
  t = seed != nullptr ? *seed : RemoteTable{};
  snprintf(t.addr, sizeof(t.addr), "%.*s", static_cast<int>(sizeof(t.addr) - 1), addr);
  return &t;
}

// Forgetting a device forgets its table too.
inline bool forgetRemote(RemoteTable* tables, uint8_t& count, const char* addr) {
  for (uint8_t i = 0; i < count; ++i) {
    if (strncmp(tables[i].addr, addr, sizeof(tables[i].addr)) != 0) continue;
    for (uint8_t j = i + 1; j < count; ++j) tables[j - 1] = tables[j];
    --count;
    return true;
  }
  return false;
}

inline bool clearAction(RemoteTable& t, const Action action) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < t.count; ++i) {
    if (actionOf(t.bindings[i]) != action) t.bindings[kept++] = t.bindings[i];
  }
  const bool changed = kept != t.count;
  t.count = kept;
  return changed;
}

// Store a learned button. An action has one button and each button + gesture
// does one thing: the old slots of both go. false when the table is full,
// never a silent overwrite.
inline bool learn(RemoteTable& t, const Action action, const uint32_t code, const Gesture gesture) {
  const Binding binding = makeBinding(code, gesture, action);
  if (!valid(binding)) return false;
  uint8_t needed = 1;
  for (uint8_t i = 0; i < t.count; ++i) {
    if (actionOf(t.bindings[i]) != action && !matches(t.bindings[i], code, gesture)) ++needed;
  }
  if (needed > kMaxBindings) return false;
  uint8_t kept = 0;
  for (uint8_t i = 0; i < t.count; ++i) {
    if (actionOf(t.bindings[i]) != action && !matches(t.bindings[i], code, gesture)) t.bindings[kept++] = t.bindings[i];
  }
  t.count = kept;
  t.bindings[t.count++] = binding;
  return true;
}
inline bool learn(RemoteTable& t, const Action action, const uint32_t code, const bool hold) {
  return learn(t, action, code, hold ? Gesture::Hold : Gesture::Tap);
}

// --- Decision for each raw edge ------------------------------------------------

enum class GesturePhase : uint8_t { Idle, Pressed, BetweenClicks, Consumed };

// Fixed-size state for one button; independent buttons never steal each other's pending clicks.
struct GestureWait {
  uint32_t code = 0;
  uint32_t atMs = 0;
  Action tap = Action::None;
  Action doubleTap = Action::None;
  Action tripleTap = Action::None;
  Action hold = Action::None;
  uint8_t clicks = 0;
  GesturePhase phase = GesturePhase::Idle;

  uint8_t maxClicks() const { return tripleTap != Action::None ? 3 : doubleTap != Action::None ? 2 : 1; }
  Action clicked() const {
    switch (clicks) {
      case 1:
        return tap;
      case 2:
        return doubleTap;
      case 3:
        return tripleTap;
      default:
        return Action::None;  // No completed click.
    }
  }
  Action finish(const Action action) {
    *this = {};
    return action;
  }
};
static_assert(sizeof(GestureWait) == 16);

// Poll before each queued edge using its timestamp, then once at the current time.
// This keeps a delayed main loop from merging clicks that were physically far apart.
inline Action pollGesture(GestureWait& w, const uint32_t nowMs) {
  switch (w.phase) {
    case GesturePhase::Pressed:
      if (w.hold != Action::None && nowMs - w.atMs >= kHoldMs) {
        w.phase = GesturePhase::Consumed;
        return w.hold;
      }
      break;
    case GesturePhase::BetweenClicks:
      if (nowMs - w.atMs >= kMultiClickMs) return w.finish(w.clicked());
      break;
    case GesturePhase::Idle:
    case GesturePhase::Consumed:
      break;
  }
  return Action::None;
}

// Buttons without multi-click/hold bindings keep their immediate press action.
// Other buttons emit only their completed gesture, never intermediate single clicks.
inline Action onRawEdge(const RemoteTable& t, const uint32_t code, const bool pressed, const uint32_t atMs,
                        const Action legacy, GestureWait& w) {
  if (pressed) {
    if (w.phase == GesturePhase::Pressed || w.phase == GesturePhase::Consumed) return Action::None;
    if (w.phase == GesturePhase::Idle) {
      w.code = code;
      const Action tap = lookup(t, code, Gesture::Tap);
      w.tap = tap != Action::None ? tap : legacy;
      w.doubleTap = lookup(t, code, Gesture::DoubleTap);
      w.tripleTap = lookup(t, code, Gesture::TripleTap);
      w.hold = lookup(t, code, Gesture::Hold);
      if (w.maxClicks() == 1 && w.hold == Action::None) return w.finish(w.tap);
    }
    w.phase = GesturePhase::Pressed;
    w.atMs = atMs;
    return Action::None;
  }
  if (w.code != code) return Action::None;
  if (w.phase == GesturePhase::Consumed) return w.finish(Action::None);
  if (w.phase != GesturePhase::Pressed) return Action::None;
  if (w.hold != Action::None && atMs - w.atMs >= kHoldMs) return w.finish(w.hold);
  ++w.clicks;
  if (w.clicks >= w.maxClicks()) return w.finish(w.clicked());
  w.phase = GesturePhase::BetweenClicks;
  w.atMs = atMs;
  return Action::None;
}

// Tick state: the table is picked ONCE per link (off the hot path) and forgotten
// when the link changes or the reader leaves the front.
struct Router {
  const RemoteTable* table = nullptr;
  bool chosen = false;
  bool linked = false;
  // At most one pending sequence per configured button; no allocation in the input path.
  std::array<GestureWait, kMaxBindings> waits{};

  GestureWait* waitFor(const uint32_t code) {
    for (auto& wait : waits) {
      if (wait.phase != GesturePhase::Idle && wait.code == code) return &wait;
    }
    for (auto& wait : waits) {
      if (wait.phase == GesturePhase::Idle) return &wait;
    }
    return nullptr;
  }

  void follow(const bool isLinked) {
    if (isLinked == linked) return;
    *this = Router();
    linked = isLinked;
  }
};

}  // namespace bleturner
