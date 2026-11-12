#include "OpdsSettingsActivity.h"

#include <GfxRenderer.h>
#include <HttpHeader.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "XuesuSettingsLayout.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#if FREEINK_DEVICE_XUESU_300
#include "activities/network/WifiProvisionActivity.h"
#endif

namespace fui = freeink::ui;

namespace {
// Editable fields: Name, URL, Username, Password, Custom Header 1, Custom
// Header 2. Existing servers also show a Delete option (BASE_ITEMS + 1).
constexpr int BASE_ITEMS = 6;
constexpr int CUSTOM_HEADER_1_INDEX = 4;
constexpr int CUSTOM_HEADER_2_INDEX = 5;
}  // namespace

OpdsSettingsActivity::OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const int serverIndex)
    : UiListActivity("OpdsSettings", renderer, mappedInput), serverIndex(serverIndex) {
  // Labels never change (unlike the values, which track editServer's fields
  // live), so they're set once here rather than every buildScreen() call.
  static constexpr StrId fieldNames[BASE_ITEMS] = {StrId::STR_SERVER_NAME,     StrId::STR_OPDS_SERVER_URL,
                                                   StrId::STR_USERNAME,        StrId::STR_PASSWORD,
                                                   StrId::STR_CUSTOM_HEADER_1, StrId::STR_CUSTOM_HEADER_2};
  for (int i = 0; i < BASE_ITEMS; i++) {
    fieldRowItems[i].label = I18N.get(fieldNames[i]);
    fieldRowItems[i].actionValue = static_cast<int16_t>(i);
  }
  fieldRowItems[BASE_ITEMS].label = tr(STR_DELETE_SERVER);
  fieldRowItems[BASE_ITEMS].actionValue = static_cast<int16_t>(BASE_ITEMS);
}

int OpdsSettingsActivity::getMenuItemCount() const {
#if FREEINK_DEVICE_XUESU_300
  return isNewServer ? 1 : 2;
#endif
  return isNewServer ? BASE_ITEMS : BASE_ITEMS + 1;  // +1 for Delete
}

void OpdsSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  isNewServer = (serverIndex < 0);
  showSaveError = false;

  if (!isNewServer) {
    // Edit flow: copy the selected server into local editable state.
    // Changes are persisted field-by-field through saveServer().
    const auto* server = OPDS_STORE.getServer(static_cast<size_t>(serverIndex));
    if (server) {
      editServer = *server;
    } else {
      // Server was deleted between navigation and entering this screen — treat as new
      isNewServer = true;
      serverIndex = -1;
    }
  }
#if FREEINK_DEVICE_XUESU_300
  fieldRowItems[0].label = tr(STR_PHONE_SETUP);
  fieldRowItems[1].label = tr(STR_DELETE_SERVER);
  fieldRowItems[1].actionValue = 1;
  if (isNewServer) openPhoneSetup();
#endif
}

#if FREEINK_DEVICE_XUESU_300
void OpdsSettingsActivity::openPhoneSetup() {
  auto form = makeUniqueNoThrow<PhoneSetupForm>();
  if (!form) {
    LOG_ERR("OPS", "OOM: phone form (%u bytes)", static_cast<unsigned>(sizeof(PhoneSetupForm)));
    return;
  }
  using Kind = PhoneSetupForm::Kind;
  form->title = tr(STR_OPDS_SERVERS);
  form->count = 6;
  form->fields[0] = {tr(STR_SERVER_NAME), editServer.name, 63};
  form->fields[1] = {tr(STR_OPDS_SERVER_URL), editServer.url, 127, Kind::Url, true};
  form->fields[2] = {tr(STR_USERNAME), editServer.username, 63};
  form->fields[3] = {tr(STR_PASSWORD), editServer.password, 63, Kind::Password};
  form->fields[4] = {tr(STR_CUSTOM_HEADER_1), formatHttpHeaderLine(editServer.customHeaders[0]), 160, Kind::Password};
  form->fields[5] = {tr(STR_CUSTOM_HEADER_2), formatHttpHeaderLine(editServer.customHeaders[1]), 160, Kind::Password};
  form->submit = [this](const PhoneSetupForm::Values& values) {
    OpdsServer candidate;
    candidate.name = values[0];
    candidate.url = values[1];
    candidate.username = values[2];
    candidate.password = values[3];
    for (size_t i = 0; i < candidate.customHeaders.size(); ++i) {
      parseHttpHeaderLine(values[4 + i], candidate.customHeaders[i]);
      if (!values[4 + i].empty() && !isValidHttpHeader(candidate.customHeaders[i]))
        return PhoneSetupForm::Result::Invalid;
    }
    const bool saved = isNewServer ? OPDS_STORE.addServer(candidate)
                                   : OPDS_STORE.updateServer(static_cast<size_t>(serverIndex), candidate);
    if (!saved) return PhoneSetupForm::Result::SaveFailed;
    editServer = std::move(candidate);
    if (isNewServer) serverIndex = static_cast<int>(OPDS_STORE.getCount()) - 1;
    isNewServer = false;
    return PhoneSetupForm::Result::Saved;
  };
  startActivityForResultWith<WifiProvisionActivity>(
      [this](const ActivityResult& result) {
        if (!result.isCancelled)
          finish();
        else
          requestUpdate();
      },
      std::move(form));
}
#endif

void OpdsSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens a keyboard or leaves the screen; a lingering flash would
  // gray an unrelated row.
  app.clearTapFlash();
  handleSelection();
}

bool OpdsSettingsActivity::saveServer() {
  bool success = false;

  if (isNewServer) {
    // Create flow: first save inserts a new server record into the multi-server store.
    success = OPDS_STORE.addServer(editServer);
    if (success) {
      // After the first successful save, promote to an existing server so
      // subsequent field edits update in-place rather than creating duplicates.
      isNewServer = false;
      serverIndex = static_cast<int>(OPDS_STORE.getCount()) - 1;
    } else {
      LOG_ERR("OPS", "Failed to add OPDS server");
    }
  } else {
    // Edit flow: update the same server entry in-place.
    success = OPDS_STORE.updateServer(static_cast<size_t>(serverIndex), editServer);
    if (!success) {
      LOG_ERR("OPS", "Failed to update OPDS server at index %d", serverIndex);
    }
  }

  showSaveError = !success;
  if (showSaveError) {
    requestUpdate();
  }

  return success;
}

void OpdsSettingsActivity::handleSelection() {
#if FREEINK_DEVICE_XUESU_300
  if (nav.selected == 0) {
    openPhoneSetup();
  } else if (nav.selected == 1 && !isNewServer) {
    if (OPDS_STORE.removeServer(static_cast<size_t>(serverIndex)))
      finish();
    else {
      showSaveError = true;
      requestUpdate();
    }
  }
  return;
#endif
  // Each field edit is saved immediately so partially configured servers
  // survive navigation and power-loss scenarios.
  if (nav.selected == 0) {
    // Server Name
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.name = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_SERVER_NAME), editServer.name, 63,
                                                      InputType::Text);
  } else if (nav.selected == 1) {
    // Server URL
    const std::string prefillUrl = editServer.url.empty() ? "https://" : editServer.url;
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_OPDS_SERVER_URL), prefillUrl, 127,
                                                      InputType::Url);
  } else if (nav.selected == 2) {
    // Username
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.username = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_USERNAME), editServer.username, 63,
                                                      InputType::Text);
  } else if (nav.selected == 3) {
    // Password
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.password = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_PASSWORD), editServer.password, 63,
                                                      InputType::Text);
  } else if (nav.selected == CUSTOM_HEADER_1_INDEX || nav.selected == CUSTOM_HEADER_2_INDEX) {
    // Custom Header 1 or 2 - single "Name: Value" line, parsed on save.
    const size_t slot = static_cast<size_t>(nav.selected == CUSTOM_HEADER_1_INDEX ? 0 : 1);
    const StrId label = nav.selected == CUSTOM_HEADER_1_INDEX ? StrId::STR_CUSTOM_HEADER_1 : StrId::STR_CUSTOM_HEADER_2;
    const std::string prefill = formatHttpHeaderLine(editServer.customHeaders[slot]);
    auto handler = [this, slot](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        parseHttpHeaderLine(kb.text, editServer.customHeaders[slot]);
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResultWith<KeyboardEntryActivity>(std::move(handler), I18N.get(label), prefill, 160,
                                                      InputType::Text);
  } else if (nav.selected == BASE_ITEMS && !isNewServer) {
    // Delete flow is only available for existing servers.
    if (!OPDS_STORE.removeServer(static_cast<size_t>(serverIndex))) {
      LOG_ERR("OPS", "Failed to remove OPDS server at index %d", serverIndex);
      showSaveError = true;
      requestUpdate();
      return;
    }
    finish();
  }
}

void OpdsSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints; derived
  // from the safe area so board bezel insets apply (same as LanguageSelect).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});

  // URL hint where the old sub-header band sat.
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
  const int16_t pad = screen.theme().headerSidePadding;
  screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), tr(STR_CALIBRE_URL_HINT), screen.theme().smallText);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // fieldRowItems' labels/actionValue were set once in the constructor; only
  // the live value pointers (already pointing at editServer's own fields, no
  // new strings built) need refreshing here.
#if FREEINK_DEVICE_XUESU_300
  fieldRowItems[0].value = nullptr;
  fieldRowItems[1].value = nullptr;
#else
  fieldRowItems[0].value = editServer.name.empty() ? tr(STR_NOT_SET) : editServer.name.c_str();
  fieldRowItems[1].value = editServer.url.empty() ? tr(STR_NOT_SET) : editServer.url.c_str();
  fieldRowItems[2].value = editServer.username.empty() ? tr(STR_NOT_SET) : editServer.username.c_str();
  fieldRowItems[3].value = editServer.password.empty() ? tr(STR_NOT_SET) : "******";
  for (size_t i = 0; i < OpdsServer::MAX_CUSTOM_HEADERS; i++) {
    headerDisplayBuf[i] = formatHttpHeaderLine(editServer.customHeaders[i], true);
    fieldRowItems[CUSTOM_HEADER_1_INDEX + i].value =
        headerDisplayBuf[i].empty() ? tr(STR_NOT_SET) : headerDisplayBuf[i].c_str();
  }
#endif

  fui::ListProps props;
  props.items = fieldRowItems;
  props.count = static_cast<uint16_t>(getMenuItemCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  if (SETTINGS.uiTheme != CrossPointSettings::INX) {
    props.labelText = screen.theme().smallText;
    props.labelText.maxLines = 2;
  }
  syncListViewport(screen, props);
  XuesuSettingsLayout::drawRows(screen, props);
}

const char* OpdsSettingsActivity::headerTitle() const {
  // Reuse STR_OPDS_BROWSER as the "edit existing server" title.
  // New server creation uses STR_ADD_SERVER.
  return isNewServer ? tr(STR_ADD_SERVER) : tr(STR_OPDS_BROWSER);
}

void OpdsSettingsActivity::drawFooter() {
  UiListActivity::drawFooter();
  if (showSaveError) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const Rect content = SubpageLayout::contentRect(safe, metrics);
    GUI.drawPopupInContent(renderer, SubpageLayout::insetHorizontal(content, metrics.contentSidePadding),
                           tr(STR_ERROR_GENERAL_FAILURE));
  }
}
