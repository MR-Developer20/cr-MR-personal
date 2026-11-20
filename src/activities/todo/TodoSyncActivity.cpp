#include "TodoSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void TodoSyncActivity::onEnter() {
  Activity::onEnter();
  // onEnter runs without the render lock; the stack takes a moment to come up.
  service.start();
  requestUpdate();
}

void TodoSyncActivity::onExit() {
  // ActivityManager already holds the render lock here.
  service.stop();
  Activity::onExit();
}

void TodoSyncActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (service.poll()) requestUpdate();
}

static const char* resultText(const TodoSyncService::Result result) {
  switch (result) {
    case TodoSyncService::Result::Synced:
      return tr(STR_TODO_SYNC_DONE);
    case TodoSyncService::Result::Conflict:
      return tr(STR_TODO_SYNC_CONFLICT);
    case TodoSyncService::Result::Rejected:
      return tr(STR_TODO_SYNC_REJECTED);
    case TodoSyncService::Result::LowMemory:
      return tr(STR_TODO_SYNC_LOW_MEMORY);
    case TodoSyncService::Result::SaveFailed:
      return tr(STR_TODO_SAVE_FAILED);
    case TodoSyncService::Result::RadioBusy:
      return tr(STR_TODO_SYNC_RADIO_BUSY);
    case TodoSyncService::Result::TimedOut:
      return tr(STR_TODO_SYNC_TIMED_OUT);
    case TodoSyncService::Result::None:
      break;
  }
  return nullptr;
}

void TodoSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TODO_SYNC));

  const TodoSyncService::State state = service.state();
  const char* status = nullptr;
  switch (state) {
    case TodoSyncService::State::Advertising:
      status = tr(STR_TODO_SYNC_WAITING);
      break;
    case TodoSyncService::State::Connected:
      status = tr(STR_TODO_SYNC_CONNECTED);
      break;
    case TodoSyncService::State::Working:
      status = tr(STR_TODO_SYNC_WORKING);
      break;
    case TodoSyncService::State::Stopped:
      break;
  }

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  int y = pageHeight / 3;
  if (status) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, status, true, EpdFontFamily::BOLD);
    y += lineHeight + metrics.verticalSpacing * 2;
  }
  const bool showCode = state == TodoSyncService::State::Advertising || state == TodoSyncService::State::Connected ||
                        state == TodoSyncService::State::Working;
  if (showCode) {
    char code[8];
    snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(service.passkey()));
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_TODO_SYNC_CODE));
    y += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_12_FONT_ID, y, code, true, EpdFontFamily::BOLD);
    y += lineHeight + metrics.verticalSpacing * 3;
  }
  if (const char* text = resultText(service.result())) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, text);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
