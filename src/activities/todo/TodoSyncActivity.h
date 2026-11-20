#pragma once

#include "TodoSyncService.h"
#include "activities/Activity.h"

/**
 * "Sync with phone" screen. Starts the Bluetooth sync service on entry, shows the pairing code and the
 * outcome of the last transfer, and stops the stack on exit. TodoActivity stays loaded beneath it, so the
 * list in TODO_STORE is the one the phone reads and replaces.
 */
class TodoSyncActivity final : public Activity {
 public:
  explicit TodoSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TodoSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return service.state() == TodoSyncService::State::Working; }
  bool preventAutoSleep() override { return service.state() != TodoSyncService::State::Stopped; }

 private:
  TodoSyncService service;
};
