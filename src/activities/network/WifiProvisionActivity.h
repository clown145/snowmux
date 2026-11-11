#pragma once

#include <BoardConfig.h>

#if FREEINK_DEVICE_XUESU_300

#include <DNSServer.h>
#include <WebServer.h>

#include <atomic>

#include "activities/Activity.h"

class WifiProvisionActivity final : public Activity {
 public:
  WifiProvisionActivity(GfxRenderer& renderer, MappedInputManager& input, bool keepStation = false)
      : Activity("WifiProvision", renderer, input), keepStation(keepStation) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class State { Starting, Ready, Connecting, Connected, Failed, SaveFailed, Unavailable };

  const bool keepStation;
  bool handoff = false;
  bool ownsRadio = false;
  std::atomic<State> state{State::Starting};
  std::atomic<bool> showPassword{false};
  DNSServer portalDns;
  std::unique_ptr<WebServer> web;
  char hotspot[24] = {};
  char hotspotPassword[13] = {};
  String apAddress;
  std::string ssid;
  std::string password;
  std::string stationAddress;
  unsigned long sessionStart = 0;
  unsigned long attemptStart = 0;
  unsigned long joinedAt = 0;

  void setState(State next);
  void startPortalDns();
  void redirectToPortal(WebServer& request) const;
  void startScan();
  void sendNetworks();
  void startConnection();
  void checkConnection();
  void sendStatus();
  void complete(bool connected);
};

#endif
