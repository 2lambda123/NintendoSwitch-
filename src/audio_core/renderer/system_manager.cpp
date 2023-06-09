// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include "audio_core/audio_core.h"
#include "audio_core/renderer/adsp/adsp.h"
#include "audio_core/renderer/system_manager.h"
#include "common/microprofile.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"

MICROPROFILE_DEFINE(Audio_RenderSystemManager, "Audio", "Render System Manager",
                    MP_RGB(60, 19, 97));

namespace AudioCore::AudioRenderer {
constexpr std::chrono::nanoseconds RENDER_TIME{5'000'000UL};

SystemManager::SystemManager(Core::System& core_)
    : core{core_}, adsp{core.AudioCore().GetADSP()}, mailbox{adsp.GetRenderMailbox()},
      thread_event{Core::Timing::CreateEvent(
          "AudioRendererSystemManager", [this](std::uintptr_t, s64 time, std::chrono::nanoseconds) {
              return ThreadFunc2(time);
          })} {}

SystemManager::~SystemManager() {
    Stop();
}

bool SystemManager::InitializeUnsafe() {
    if (!active) {
        if (adsp.Start()) {
            active = true;
            thread = std::jthread([this](std::stop_token stop_token) { ThreadFunc(); });
            core.CoreTiming().ScheduleLoopingEvent(std::chrono::nanoseconds(0), RENDER_TIME,
                                                   thread_event);
        }
    }

    return adsp.GetState() == ADSP::State::Started;
}

void SystemManager::Stop() {
    if (!active) {
        return;
    }
    core.CoreTiming().UnscheduleEvent(thread_event, {});
    active = false;
    update.store(true);
    update.notify_all();
    thread.join();
    adsp.Stop();
}

bool SystemManager::Add(System& system_) {
    std::scoped_lock l2{mutex2};

    if (systems.size() + 1 > MaxRendererSessions) {
        LOG_ERROR(Service_Audio, "Maximum AudioRenderer Systems active, cannot add more!");
        return false;
    }

    {
        std::scoped_lock l{mutex1};
        if (systems.empty()) {
            if (!InitializeUnsafe()) {
                LOG_ERROR(Service_Audio, "Failed to start the AudioRenderer SystemManager");
                return false;
            }
        }
    }

    systems.push_back(&system_);
    return true;
}

bool SystemManager::Remove(System& system_) {
    std::scoped_lock l2{mutex2};

    {
        std::scoped_lock l{mutex1};
        if (systems.remove(&system_) == 0) {
            LOG_ERROR(Service_Audio,
                      "Failed to remove a render system, it was not found in the list!");
            return false;
        }
    }

    if (systems.empty()) {
        Stop();
    }
    return true;
}

void SystemManager::ThreadFunc() {
    static constexpr char name[]{"AudioRenderSystemManager"};
    MicroProfileOnThreadCreate(name);
    Common::SetCurrentThreadName(name);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    while (active) {
        {
            std::scoped_lock l{mutex1};

            MICROPROFILE_SCOPE(Audio_RenderSystemManager);

            for (auto system : systems) {
                system->SendCommandToDsp();
            }
        }

        adsp.Signal();
        adsp.Wait();

        update.wait(false);
        update.store(false);
    }
}

std::optional<std::chrono::nanoseconds> SystemManager::ThreadFunc2(s64 time) {
    update.store(true);
    update.notify_all();
    return std::nullopt;
}

} // namespace AudioCore::AudioRenderer
