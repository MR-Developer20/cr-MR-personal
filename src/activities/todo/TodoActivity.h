#pragma once

#include <TodoList.h>

#include <cstdint>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

/**
 * To-do list screen.
 *
 * Rows are the live items followed by a virtual "+ Add item" row and, in builds with Bluetooth, a
 * "Sync with phone" row.
 *   Confirm / tap   on an item: check / uncheck it
 *   Confirm / tap   on "+ Add item": open the keyboard to add one
 *   Confirm / tap   on "Sync with phone": open the Bluetooth sync screen
 *   Hold Confirm / long-press on an item: options popup (edit, move, delete, delete completed)
 *   Back: return home
 *
 * The list is loaded from SD in onEnter() and saved + freed in onExit(); edits are also
 * flushed after SAVE_IDLE_MS without input so a crash or battery pull loses little.
 * Rows are resolved from the store through ListProps::rowProvider, so no per-row copy exists.
 */
class TodoActivity final : public UiListActivity {
 public:
  explicit TodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;

 private:
  enum class ItemAction : uint8_t { Edit, MoveUp, MoveDown, Delete, DeleteDone };
  static constexpr int MAX_MENU_ACTIONS = 5;
  static constexpr unsigned long LONG_PRESS_MS = 1000;  // Same hold threshold as the file browser.
  static constexpr unsigned long SAVE_IDLE_MS = 10000;

  OptionPopup optionPopup;
  ItemAction menuActions[MAX_MENU_ACTIONS] = {};
  int menuActionCount = 0;
  unsigned long lastEditMs = 0;

  // UiListActivity contract.
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void onBackButton() override;
  void drawChrome() override;
  void drawFooter() override;

  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  bool isItemRow(int index) const;
  bool isAddRow(int index) const;
  bool isSyncRow(int index) const;
  static bool syncAvailable();
  void clampSelection();

  void openMenu();
  void runMenuAction(ItemAction action);
  void openKeyboard(int itemIndex);  // -1 adds a new item.
  void openSync();
  void promptDeleteDone();
  void showMessage(const char* message);
  // Caller must hold the RenderLock.
  void afterEdit(TodoEditResult result);
  void saveIfIdle();
};
