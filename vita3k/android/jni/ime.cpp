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

#include <dialog/state.h>
#include <ime/keyboard.h>
#include <ime/state.h>
#include <util/string_utils.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_system.h>
#include <jni.h>

#include <atomic>
#include <mutex>
#include <string>

namespace ime {

// Written by the SDL thread when the window is created/destroyed and read by guest
// threads from SceIme / SceCommonDialog.
static std::atomic<SDL_Window *> s_window{ nullptr };

namespace {

// The IME pushes run on every text editing event, so the activity class and its method
// ids are resolved once instead of doing a string-based lookup per call.
struct ImeJniCache {
    // Written by the SDL thread (on_activity_recreated) and read by guest threads, so
    // every access to the members below is guarded by this.
    std::mutex mutex;

    jclass clazz = nullptr;
    jmethodID update_state = nullptr;
    jmethodID clear_state = nullptr;
    jmethodID set_keyboard_active = nullptr;
    // Last pushed state, used to skip redundant hops to the UI thread.
    bool has_last = false;
    bool last_sce_ime_active = false;
    bool last_dialog_active = false;
    bool last_multiline = false;
    uint32_t last_preedit_start = 0;
    uint32_t last_preedit_length = 0;
    uint32_t last_caret_index = 0;
    std::string last_text;
    std::string last_enter_label;
};

ImeJniCache &jni_cache() {
    static ImeJniCache cache;
    return cache;
}

void clear_ime_state(JNIEnv *env, jobject activity) {
    jmethodID method_id = nullptr;
    {
        auto &cache = jni_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);

        if (!cache.clazz || !cache.clear_state)
            return;

        if (cache.has_last && !cache.last_sce_ime_active && !cache.last_dialog_active) {
            // Already cleared on the Java side.
            return;
        }

        method_id = cache.clear_state;
    }

    // Deliberately outside the lock: Java can call back into the emulator, and holding
    // the cache mutex across that hop would be a deadlock waiting to happen.
    env->CallVoidMethod(activity, method_id);

    auto &cache = jni_cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.has_last = true;
    cache.last_sce_ime_active = false;
    cache.last_dialog_active = false;
    cache.last_text.clear();
    cache.last_enter_label.clear();
    cache.last_preedit_start = 0;
    cache.last_preedit_length = 0;
    cache.last_caret_index = 0;
    cache.last_multiline = false;
}

void push_ime_state(JNIEnv *env, jobject activity, EmuEnvState &emuenv) {
    const bool dialog_active = is_ime_dialog_active(emuenv);
    const bool sce_ime_active = emuenv.ime.state;

    if (!sce_ime_active && !dialog_active) {
        clear_ime_state(env, activity);
        return;
    }

    {
        auto &cache = jni_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (!cache.clazz || !cache.update_state)
            return;
    }

    std::u16string text;
    uint32_t preedit_start = 0;
    uint32_t preedit_length = 0;
    uint32_t caret_index = 0;
    bool multiline = false;
    std::string enter_label;
    {
        std::lock_guard<std::recursive_mutex> dialog_lock(emuenv.common_dialog.mutex);
        std::lock_guard<std::mutex> ime_lock(emuenv.ime.mutex);
        text = emuenv.ime.str;
        preedit_start = emuenv.ime.edit_text.preeditIndex;
        preedit_length = emuenv.ime.edit_text.preeditLength;
        caret_index = emuenv.ime.edit_text.caretIndex;
        multiline = dialog_active
            ? emuenv.common_dialog.ime.multiline
            : ((emuenv.ime.param.option & SCE_IME_OPTION_MULTILINE) != 0);
        enter_label = emuenv.ime.enter_label;
    }

    const std::string utf8_text = string_utils::utf16_to_utf8(text);

    jmethodID update_state = nullptr;
    {
        auto &cache = jni_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);

        // Preedit events arrive in bursts; converting the whole buffer and hopping to the
        // UI thread for an unchanged state is pure waste.
        if (cache.has_last
            && cache.last_sce_ime_active == sce_ime_active
            && cache.last_dialog_active == dialog_active
            && cache.last_multiline == multiline
            && cache.last_preedit_start == preedit_start
            && cache.last_preedit_length == preedit_length
            && cache.last_caret_index == caret_index
            && cache.last_text == utf8_text
            && cache.last_enter_label == enter_label) {
            return;
        }

        update_state = cache.update_state;
    }

    jstring text_value = env->NewStringUTF(utf8_text.c_str());
    jstring enter_label_value = env->NewStringUTF(enter_label.c_str());
    // Outside the cache lock: Java re-enters the emulator through other JNI entry points.
    env->CallVoidMethod(activity,
        update_state,
        static_cast<jboolean>(sce_ime_active),
        static_cast<jboolean>(dialog_active),
        text_value,
        static_cast<jint>(preedit_start),
        static_cast<jint>(preedit_length),
        static_cast<jint>(caret_index),
        static_cast<jboolean>(multiline),
        enter_label_value);
    env->DeleteLocalRef(text_value);
    env->DeleteLocalRef(enter_label_value);

    auto &cache = jni_cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.has_last = true;
    cache.last_sce_ime_active = sce_ime_active;
    cache.last_dialog_active = dialog_active;
    cache.last_multiline = multiline;
    cache.last_preedit_start = preedit_start;
    cache.last_preedit_length = preedit_length;
    cache.last_caret_index = caret_index;
    cache.last_text = utf8_text;
    cache.last_enter_label = enter_label;
}

// Resolves the activity class and its methods once per process.
bool ensure_jni_cache(JNIEnv *env, jobject activity) {
    auto &cache = jni_cache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.clazz)
            return true;
    }

    jclass local_clazz = env->GetObjectClass(activity);
    if (!local_clazz)
        return false;

    jmethodID update_state = env->GetMethodID(local_clazz,
        "updateNativeImeState",
        "(ZZLjava/lang/String;IIIZLjava/lang/String;)V");
    jmethodID clear_state = env->GetMethodID(local_clazz, "clearNativeImeState", "()V");
    jmethodID set_keyboard_active = env->GetMethodID(local_clazz, "setKeyboardActive", "(Z)V");

    // A global ref keeps the cache valid across threads; it is dropped again when the
    // activity is destroyed, otherwise every session restart would leak one.
    jclass global_clazz = reinterpret_cast<jclass>(env->NewGlobalRef(local_clazz));
    env->DeleteLocalRef(local_clazz);

    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.clazz) {
        // Another thread won the race; keep its result and drop ours.
        if (global_clazz)
            env->DeleteGlobalRef(global_clazz);
        return true;
    }

    cache.update_state = update_state;
    cache.clear_state = clear_state;
    cache.set_keyboard_active = set_keyboard_active;
    cache.clazz = global_clazz;
    cache.has_last = false;

    return cache.clazz != nullptr;
}

void invalidate_jni_cache() {
    auto &cache = jni_cache();
    jclass stale = nullptr;

    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        stale = cache.clazz;
        cache.clazz = nullptr;
        cache.update_state = nullptr;
        cache.clear_state = nullptr;
        cache.set_keyboard_active = nullptr;
        cache.has_last = false;
        cache.last_text.clear();
        cache.last_enter_label.clear();
    }

    JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    if (env && stale)
        env->DeleteGlobalRef(stale);
}

} // namespace

void set_sdl_window(SDL_Window *window) {
    s_window.store(window, std::memory_order_release);
}

void set_keyboard_active(bool active) {
    if (SDL_Window *window = s_window.load(std::memory_order_acquire)) {
        if (active)
            SDL_StartTextInput(window);
        else
            SDL_StopTextInput(window);
    }

    JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return;

    if (!ensure_jni_cache(env, activity))
        return;

    jmethodID set_keyboard_active = nullptr;
    {
        auto &cache = jni_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        set_keyboard_active = cache.set_keyboard_active;
    }

    if (set_keyboard_active)
        env->CallVoidMethod(activity, set_keyboard_active, static_cast<jboolean>(active));

    auto *emuenv = get_emuenv();
    if (emuenv)
        push_ime_state(env, activity, *emuenv);
    else
        clear_ime_state(env, activity);
}

void notify_ime_state_changed() {
    JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return;

    if (!ensure_jni_cache(env, activity))
        return;

    auto *emuenv = get_emuenv();
    if (emuenv)
        push_ime_state(env, activity, *emuenv);
    else
        clear_ime_state(env, activity);
}

// The activity object changes across a configuration change; the cached global ref then
// points at the destroyed instance, so it has to be dropped and resolved again.
void on_activity_recreated() {
    invalidate_jni_cache();
}

} // namespace ime
