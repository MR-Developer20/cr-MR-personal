#include "TodoActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <string>
#include <variant>

#include "MappedInputManager.h"
#include "TodoStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

TodoActivity::TodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Todo", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void TodoActivity::onEnter() {
  {
    // onEnter runs without the render lock; SD shares SPI with the display. Load before the base
    // class requests its first render so the first paint already shows the list.
    RenderLock lock(*this);
    TODO_STORE.load();
  }
  lastEditMs = 0;
  UiListActivity::onEnter();
  LOG_DBG("TODO", "free heap %u after load", static_cast<unsigned>(ESP.getFreeHeap()));
}

void TodoActivity::onExit() {
  // ActivityManager already holds the render lock here (exitActivity / stack unwind),
  // and RenderLock is not recursive, so don't take it again.
  optionPopup.dismiss();
  TODO_STORE.saveIfDirty();
  TODO_STORE.unload();
  LOG_DBG("TODO", "free heap %u after unload", static_cast<unsigned>(ESP.getFreeHeap()));
  UiListActivity::onExit();
}

// ---------------------------------------------------------------------------
// Rows and input
// ---------------------------------------------------------------------------

int TodoActivity::listCount() const { return static_cast<int>(TODO_STORE.list().size()) + 1; }

bool TodoActivity::isAddRow(const int index) const { return index >= static_cast<int>(TODO_STORE.list().size()); }

void TodoActivity::clampSelection() {
  const int last = listCount() - 1;
  if (nav.selected > last) moveSelectionTo(last);
  if (nav.selected < 0) moveSelectionTo(0);
}

// Runs on the render task (inside the render lock), once per visible row.
void TodoActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<TodoActivity*>(ctx);
  const auto& list = TODO_STORE.list();
  if (index < list.size()) {
    const TodoItem& todo = list.at(index);
    item.label = todo.text.c_str();
    GUI.setCheckboxRow(item, todo.done);
  } else if (self->isAddRow(index)) {
    item.label = tr(STR_TODO_ADD_ITEM);
  }
  item.actionValue = static_cast<int16_t>(index);
}

void TodoActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.rowProvider = &TodoActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  // Tap checks/adds; long-press opens the item options (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}

void TodoActivity::activateIndex(const int index) {
  nav.selected = index;
  app.clearTapFlash();
  // Loop task: take the lock around anything the render task reads.
  RenderLock lock(*this);
  if (isAddRow(index)) {
    // Say so now, while the list is on screen, rather than after the user has typed an item.
    if (TODO_STORE.list().isFull()) {
      afterEdit(TodoEditResult::ListFull);
      return;
    }
    // Opening the keyboard only queues a push; it does not render under the lock.
    openKeyboard(-1);
    return;
  }
  afterEdit(TODO_STORE.list().toggleDone(static_cast<size_t>(index)));
}

void TodoActivity::onRowLongPress(const int index) {
  if (isAddRow(index)) return;
  nav.selected = index;
  openMenu();
}

bool TodoActivity::handleCustomInput() {
  // The popup, while open, consumes the whole pass.
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  saveIfIdle();
  return false;
}

bool TodoActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    // The matching release is suppressed by wasLongPressed(), so it won't also toggle the item.
    if (!isAddRow(nav.selected)) {
      app.clearTapFlash();
      openMenu();
    }
    return true;
  }
  return UiListActivity::handleButtons();
}

void TodoActivity::onBackButton() { onGoHome(); }

// ---------------------------------------------------------------------------
// Item options
// ---------------------------------------------------------------------------

void TodoActivity::openMenu() {
  if (optionPopup.isActive() || isAddRow(nav.selected)) return;
  const auto& list = TODO_STORE.list();
  const auto index = static_cast<size_t>(nav.selected);

  StrId labels[MAX_MENU_ACTIONS];
  menuActionCount = 0;
  const auto addAction = [&](const ItemAction action, const StrId label) {
    menuActions[menuActionCount] = action;
    labels[menuActionCount] = label;
    ++menuActionCount;
  };
  addAction(ItemAction::Edit, StrId::STR_TODO_EDIT);
  if (index > 0) addAction(ItemAction::MoveUp, StrId::STR_TODO_MOVE_UP);
  if (index + 1 < list.size()) addAction(ItemAction::MoveDown, StrId::STR_TODO_MOVE_DOWN);
  addAction(ItemAction::Delete, StrId::STR_DELETE);
  if (list.doneCount() > 0) addAction(ItemAction::DeleteDone, StrId::STR_TODO_DELETE_DONE);

  // show() copies the labels, so the local array may go out of scope.
  optionPopup.show(StrId::STR_TODO_OPTIONS, labels, menuActionCount, 0, [this](const int picked) {
    if (picked >= 0 && picked < menuActionCount) runMenuAction(menuActions[picked]);
  });
  requestUpdate();
}

void TodoActivity::runMenuAction(const ItemAction action) {
  const auto index = static_cast<size_t>(nav.selected);
  switch (action) {
    case ItemAction::Edit:
      openKeyboard(nav.selected);
      break;
    case ItemAction::MoveUp: {
      RenderLock lock(*this);
      const auto result = TODO_STORE.list().moveUp(index);
      afterEdit(result);
      if (result == TodoEditResult::Ok) moveSelectionTo(nav.selected - 1);
      break;
    }
    case ItemAction::MoveDown: {
      RenderLock lock(*this);
      const auto result = TODO_STORE.list().moveDown(index);
      afterEdit(result);
      if (result == TodoEditResult::Ok) moveSelectionTo(nav.selected + 1);
      break;
    }
    case ItemAction::Delete: {
      // The hold + menu pick is already a deliberate two-step action, so no extra prompt.
      RenderLock lock(*this);
      afterEdit(TODO_STORE.list().remove(index));
      clampSelection();
      break;
    }
    case ItemAction::DeleteDone:
      promptDeleteDone();
      break;
  }
}

void TodoActivity::openKeyboard(const int itemIndex) {
  const bool adding = itemIndex < 0;
  std::string initialText;
  if (!adding) initialText = TODO_STORE.list().at(static_cast<size_t>(itemIndex)).text;

  auto handler = [this, itemIndex, adding](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto& keyboard = std::get<KeyboardResult>(result.data);
    // Result handlers run without the render lock.
    RenderLock lock(*this);
    auto& list = TODO_STORE.list();
    const auto editResult =
        adding ? list.add(keyboard.text) : list.setText(static_cast<size_t>(itemIndex), keyboard.text);
    afterEdit(editResult);
    // Keep the cursor on "+ Add item" so several items can be added in a row.
    if (adding && editResult == TodoEditResult::Ok) moveSelectionTo(static_cast<int>(list.size()));
  };

  auto keyboard =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, adding ? tr(STR_TODO_NEW) : tr(STR_TODO_EDIT),
                                               initialText, TodoList::MAX_TEXT_BYTES, InputType::Text);
  if (!keyboard) {
    LOG_ERR("TODO", "OOM: keyboard activity");
    return;
  }
  startActivityForResult(std::move(keyboard), std::move(handler));
}

void TodoActivity::promptDeleteDone() {
  char body[48];
  snprintf(body, sizeof(body), tr(STR_TODO_DONE_COUNT), static_cast<int>(TODO_STORE.list().doneCount()));

  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    RenderLock lock(*this);
    if (TODO_STORE.list().removeDone() > 0) afterEdit(TodoEditResult::Ok);
    clampSelection();
  };
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_TODO_DELETE_DONE_CONFIRM), body);
  if (!confirmation) {
    LOG_ERR("TODO", "OOM: confirmation activity");
    return;
  }
  startActivityForResult(std::move(confirmation), std::move(handler));
}

void TodoActivity::showMessage(const char* message) {
  const char* options[] = {tr(STR_CONFIRM)};
  optionPopup.showMessage(tr(STR_TODO_TITLE), message, options, 1, 0, nullptr);
}

void TodoActivity::afterEdit(const TodoEditResult result) {
  switch (result) {
    case TodoEditResult::Ok:
      TODO_STORE.markDirty();
      lastEditMs = millis();
      break;
    case TodoEditResult::ListFull:
      showMessage(tr(STR_TODO_LIST_FULL));
      break;
    case TodoEditResult::NoChange:
    case TodoEditResult::EmptyText:
    case TodoEditResult::BadIndex:
      break;
  }
  requestUpdate();
}

void TodoActivity::saveIfIdle() {
  if (!TODO_STORE.isDirty() || millis() - lastEditMs < SAVE_IDLE_MS) return;
  RenderLock lock(*this);
  if (!TODO_STORE.saveIfDirty()) {
    // Back off instead of retrying the SD write on every loop iteration.
    lastEditMs = millis();
    showMessage(tr(STR_TODO_SAVE_FAILED));
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void TodoActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& list = TODO_STORE.list();
  const int itemCount = static_cast<int>(list.size());

  char progress[40] = {};
  if (itemCount > 0) {
    snprintf(progress, sizeof(progress), tr(STR_TODO_PROGRESS), static_cast<int>(list.doneCount()), itemCount);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_TODO_TITLE), itemCount > 0 ? progress : nullptr);
}

void TodoActivity::drawFooter() {
  const char* confirmLabel = tr(STR_TODO_ADD);
  if (!isAddRow(nav.selected)) {
    confirmLabel =
        TODO_STORE.list().at(static_cast<size_t>(nav.selected)).done ? tr(STR_TODO_UNCHECK) : tr(STR_TODO_CHECK);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TodoActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
