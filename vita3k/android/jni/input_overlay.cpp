// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "android_state.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_log.h>
#include <ctrl/functions.h>
#include <jni.h>
#include <touch/functions.h>

#include <atomic>
#include <mutex>

// Attached from the UI thread (InputOverlay) and closed from the SDL thread during
// session teardown, so both the handle and the id are published atomically.
static std::atomic<int> virtual_joystick_id{ -1 };
static std::atomic<SDL_Joystick *> virtual_joystick{ nullptr };
// Closing a joystick must not race with axis/button writes coming from the overlay.
static std::mutex virtual_joystick_mutex;

static void refresh_overlay_controllers() {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    if (!emuenv || !controller || !controller->has_active_session() || !SDL_WasInit(SDL_INIT_GAMEPAD))
        return;

    refresh_controllers(emuenv->ctrl, *emuenv);
}

void detach_overlay_virtual_controller() {
    {
        std::lock_guard<std::mutex> lock(virtual_joystick_mutex);
        if (SDL_Joystick *joystick = virtual_joystick.exchange(nullptr))
            SDL_CloseJoystick(joystick);
        if (const int id = virtual_joystick_id.exchange(-1); id >= 0)
            SDL_DetachVirtualJoystick(id);
    }

    refresh_overlay_controllers();
}

void attach_overlay_virtual_controller() {
    detach_overlay_virtual_controller();

    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        return;

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Vita3K Virtual Controller";

    const int id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        SDL_Log("Could not create overlay virtual controller: %s", SDL_GetError());
        return;
    }

    SDL_Joystick *joystick = SDL_OpenJoystick(id);
    if (!joystick) {
        SDL_Log("Could not create virtual joystick: %s", SDL_GetError());
        SDL_DetachVirtualJoystick(id);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(virtual_joystick_mutex);
        virtual_joystick_id.store(id, std::memory_order_release);
        virtual_joystick.store(joystick, std::memory_order_release);
    }

    refresh_overlay_controllers();
}

extern "C" {

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_attachController(JNIEnv *env, jobject thiz) {
    attach_overlay_virtual_controller();
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_detachController(JNIEnv *env, jobject thiz) {
    detach_overlay_virtual_controller();
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_setAxis(JNIEnv *env, jobject thiz, jint axis, jshort value) {
    SDL_Joystick *joystick = virtual_joystick.load(std::memory_order_acquire);
    if (!joystick)
        return;

    // Held for the duration of the write so detach cannot close the handle mid-update.
    std::lock_guard<std::mutex> lock(virtual_joystick_mutex);
    if (virtual_joystick.load(std::memory_order_relaxed) != joystick)
        return;

    SDL_SetJoystickVirtualAxis(joystick, axis, value);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_setButton(JNIEnv *env, jobject thiz, jint button, jboolean value) {
    SDL_Joystick *joystick = virtual_joystick.load(std::memory_order_acquire);
    if (!joystick)
        return;

    std::lock_guard<std::mutex> lock(virtual_joystick_mutex);
    if (virtual_joystick.load(std::memory_order_relaxed) != joystick)
        return;

    if (button < 0)
        // l2/r2
        SDL_SetJoystickVirtualAxis(joystick, -button, value ? SDL_MAX_SINT16 : 0);
    else
        SDL_SetJoystickVirtualButton(joystick, button, value);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_setTouchState(JNIEnv *env, jobject thiz, jboolean is_back) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return;

    set_rear_touchscreen(emuenv->touch, static_cast<bool>(is_back));
}

} // extern "C"
