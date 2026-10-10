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
#include <ime/state.h>
#include <io/state.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace {

AndroidSessionState session_state;

// Progress reporting state. The install loops report per file and per PFS chunk, and each
// callback allocates a jstring before hopping to Java, which pushes it into a foreground
// service notification - a binder call into system_server. Status text is almost always a
// handful of fixed strings, so the matching jstring is cached as a global ref and reused.
struct ProgressCache {
    std::mutex mutex;
    bool has_last = false;
    int last_percent = -1;
    std::string last_status;
    jstring jstatus = nullptr;
};

ProgressCache &progress_cache() {
    static ProgressCache cache;
    return cache;
}

} // namespace

AndroidSessionState &android_session_state() {
    return session_state;
}

EmuEnvState *get_emuenv() {
    return android_session_state().emuenv.get();
}

app::AppSessionController *get_app_session_controller() {
    return android_session_state().app_session_controller.get();
}

Root &get_root_paths() {
    return android_session_state().root_paths;
}

std::string jstring_to_string(JNIEnv *env, jstring str) {
    const char *utf = env->GetStringUTFChars(str, nullptr);
    std::string result(utf);
    env->ReleaseStringUTFChars(str, utf);
    return result;
}

bool is_ime_dialog_active(const EmuEnvState &emuenv) {
    return emuenv.common_dialog.type == IME_DIALOG
        && emuenv.common_dialog.status == SCE_COMMON_DIALOG_STATUS_RUNNING;
}

bool is_any_ime_active(const EmuEnvState &emuenv) {
    return emuenv.ime.state || is_ime_dialog_active(emuenv);
}

void finish_ime_dialog(EmuEnvState &emuenv) {
    auto &dialog = emuenv.common_dialog;
    auto &ime = emuenv.ime;

    std::lock_guard<std::recursive_mutex> dialog_lock(dialog.mutex);
    std::lock_guard<std::mutex> ime_lock(ime.mutex);

    const size_t copy_len = std::min(static_cast<size_t>(ime.str.length()),
        static_cast<size_t>(dialog.ime.max_length));
    if (dialog.ime.result) {
        std::memcpy(dialog.ime.result, ime.str.c_str(), copy_len * sizeof(uint16_t));
        dialog.ime.result[copy_len] = 0;
    }

    const std::string utf8 = string_utils::utf16_to_utf8(ime.str);
    std::snprintf(dialog.ime.text, sizeof(dialog.ime.text), "%s", utf8.c_str());
    dialog.ime.status = SCE_IME_DIALOG_BUTTON_ENTER;
    dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
    dialog.result = SCE_COMMON_DIALOG_RESULT_OK;
}

void cancel_ime_dialog(EmuEnvState &emuenv) {
    auto &dialog = emuenv.common_dialog;
    if (!dialog.ime.cancelable)
        return;

    std::lock_guard<std::recursive_mutex> dialog_lock(dialog.mutex);
    dialog.ime.status = SCE_IME_DIALOG_BUTTON_CLOSE;
    dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
    dialog.result = SCE_COMMON_DIALOG_RESULT_USER_CANCELED;
}

bool submit_current_ime(EmuEnvState &emuenv) {
    if (!is_any_ime_active(emuenv))
        return false;

    if (is_ime_dialog_active(emuenv)) {
        finish_ime_dialog(emuenv);
    } else {
        std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        emuenv.ime.event_id = SCE_IME_EVENT_PRESS_ENTER;
    }

    return true;
}

bool dismiss_current_ime(EmuEnvState &emuenv) {
    if (!is_any_ime_active(emuenv))
        return false;

    if (is_ime_dialog_active(emuenv)) {
        cancel_ime_dialog(emuenv);
        if (emuenv.common_dialog.status != SCE_COMMON_DIALOG_STATUS_FINISHED)
            return false;
    } else {
        std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        emuenv.ime.event_id = SCE_IME_EVENT_PRESS_CLOSE;
    }

    return true;
}

ScopedJniCallback::ScopedJniCallback(JNIEnv *env, jobject callback_obj, const char *method_name, const char *method_sig)
    : env_(env) {
    env->GetJavaVM(&java_vm_);
    callback_ = env->NewGlobalRef(callback_obj);
    jclass callback_class = env->GetObjectClass(callback_obj);
    method_ = env->GetMethodID(callback_class, method_name, method_sig);
    env->DeleteLocalRef(callback_class);
}

ScopedJniCallback::~ScopedJniCallback() {
    if (!callback_ || !java_vm_)
        return;

    JNIEnv *env = nullptr;
    bool attached = false;
    const jint result = java_vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        attached = java_vm_->AttachCurrentThread(&env, nullptr) == JNI_OK;
        if (!env)
            return;
    } else if (result != JNI_OK) {
        return;
    }

    env->DeleteGlobalRef(callback_);
    callback_ = nullptr;

    // Drop the cached status string so it is not leaked into the next install.
    auto &cache = progress_cache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.jstatus) {
            env->DeleteGlobalRef(cache.jstatus);
            cache.jstatus = nullptr;
        }
        cache.has_last = false;
        cache.last_percent = -1;
        cache.last_status.clear();
    }

    if (attached)
        java_vm_->DetachCurrentThread();
}

void ScopedJniCallback::call_progress(int percent, const char *status) const {
    if (!callback_ || !method_ || !env_)
        return;

    const int clamped = std::clamp(percent, 0, 100);
    const std::string status_text = status ? status : "";

    jstring arg = nullptr;
    // Owned by this function alone. The cached global ref is never handed to Java directly:
    // the cache is process-wide and another install finishing could delete it while this
    // thread is still inside CallVoidMethod.
    {
        auto &cache = progress_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);

        // Consecutive ticks carrying the same value are common - a per-file loop over a
        // large archive reports them constantly - and each one used to cost a jstring
        // allocation plus a notification update in system_server.
        if (cache.has_last && cache.last_percent == clamped && cache.last_status == status_text)
            return;

        arg = env_->NewStringUTF(status_text.c_str());

        if (cache.jstatus) {
            env_->DeleteGlobalRef(cache.jstatus);
            cache.jstatus = nullptr;
        }
        if (arg)
            cache.jstatus = reinterpret_cast<jstring>(env_->NewGlobalRef(arg));
    }

    // Outside the lock: the Java callback can re-enter the emulator through other JNI
    // entry points that take this same mutex.
    if (!arg) {
        // Out of memory. The tick is not recorded, so the next one with the same values
        // still reaches the listener instead of being swallowed as a duplicate.
        return;
    }

    env_->CallVoidMethod(callback_, method_, static_cast<jint>(clamped), arg);

    const bool threw = env_->ExceptionCheck() != JNI_FALSE;

    env_->DeleteLocalRef(arg);

    if (threw) {
        env_->ExceptionDescribe();
        env_->ExceptionClear();

        // The listener threw, so it is no longer usable: drop the cached string and stop
        // remembering the last tick, which makes the next call retry the listener once and
        // then give up quietly if it throws again.
        auto &cache = progress_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.jstatus) {
            env_->DeleteGlobalRef(cache.jstatus);
            cache.jstatus = nullptr;
        }
        cache.has_last = false;
        cache.last_percent = -1;
        cache.last_status.clear();
        return;
    }

    auto &cache = progress_cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.has_last = true;
    cache.last_percent = clamped;
    cache.last_status = status_text;
}

namespace {

// Title-id index over the apps list. app::get_apps() returns the whole vector by value
// under a mutex, and find_app_by_title_id is called per list item on every long-press and
// per info sheet, so the copy added up.
struct AppsIndex {
    std::mutex mutex;
    std::unordered_map<std::string, app::AppEntry> by_title_id;
    // Which user generation the index was built for.
    std::string user_id;
};

AppsIndex &apps_index() {
    static AppsIndex index;
    return index;
}

// Filesystem-derived per-app values, keyed by title id. Cleared wholesale whenever
// something changes the paths they were derived from.
struct AppInfoCache {
    std::mutex mutex;
    std::unordered_map<std::string, uint32_t> action_mask;
    std::unordered_map<std::string, uint64_t> install_size;
};

AppInfoCache &app_info_cache() {
    static AppInfoCache cache;
    return cache;
}

// Returning a reference into the index would outlive the lock, and the caller reads it
// after this function returns while another thread may be rebuilding the map.
std::optional<app::AppEntry> lookup_app_by_title_id(const EmuEnvState &emuenv, const std::string &title_id) {
    auto &index = apps_index();
    std::lock_guard<std::mutex> lock(index.mutex);

    const auto &list = emuenv.app.apps_list;
    // The list is copied under its own mutex, matching how app::get_apps() does it. A
    // reference into it must not escape this scope, hence the copy below.
    std::lock_guard<std::mutex> list_lock(list.mutex);

    // Rebuilt when the list size or the active user changes; app paths and titles are
    // otherwise stable for a session.
    if (index.by_title_id.size() != list.apps.size() || index.user_id != emuenv.io.user_id) {
        std::unordered_map<std::string, app::AppEntry> rebuilt;
        rebuilt.reserve(list.apps.size());
        for (const auto &entry : list.apps) {
            rebuilt.emplace(entry.title_id, entry);
        }
        index.by_title_id = std::move(rebuilt);
        index.user_id = emuenv.io.user_id;
    }

    const auto it = index.by_title_id.find(title_id);
    if (it == index.by_title_id.end())
        return std::nullopt;

    return it->second;
}

} // namespace

void invalidate_app_info_cache() {
    {
        auto &cache = app_info_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.action_mask.clear();
        cache.install_size.clear();
    }

    // The apps list itself is about to change, so the index has to be dropped as well.
    // Comparing list sizes is not enough: deleting one app and installing another keeps
    // the count identical while every entry in the index becomes stale.
    auto &index = apps_index();
    std::lock_guard<std::mutex> lock(index.mutex);
    index.by_title_id.clear();
}

// Cached install size for an app, computed on first request.
uint64_t get_app_install_size(const std::string &title_id, const fs::path &app_path) {
    {
        auto &cache = app_info_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto it = cache.install_size.find(title_id);
        if (it != cache.install_size.end())
            return it->second;
    }

    if (!fs::exists(app_path))
        return 0;

    uint64_t size = 0;
    try {
        for (const auto &entry : fs::recursive_directory_iterator(app_path, fs::directory_options::skip_permission_denied)) {
            boost::system::error_code error;
            if (entry.is_regular_file(error)) {
                const auto file_size = fs::file_size(entry.path(), error);
                if (!error)
                    size += file_size;
            }
        }
    } catch (const std::exception &exception) {
        LOG_WARN("Failed to compute install size for {}: {}", title_id, exception.what());
    }

    auto &cache = app_info_cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.install_size[title_id] = size;
    return size;
}

std::optional<app::AppEntry> find_app_by_title_id(const std::string &title_id) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return std::nullopt;

    return lookup_app_by_title_id(*emuenv, title_id);
}

bool remove_path_if_exists(const fs::path &path) {
    if (!fs::exists(path))
        return false;

    try {
        fs::remove_all(path);
        return true;
    } catch (const std::exception &error) {
        LOG_WARN("Failed to remove '{}': {}", path.string(), error.what());
        return false;
    }
}

uint32_t get_app_action_availability_mask(const std::string &title_id) {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    if (!emuenv)
        return 0;

    // Everything except the delete-app bit is a pure filesystem question, so it is cached
    // and paid once per title id. ACTION_DELETE_APPLICATION is not: it also depends on
    // whether a session is running, and starting or stopping a game does not invalidate
    // this cache, so that single bit is recomputed on every call instead.
    const bool session_active = controller && controller->has_active_session();

    uint32_t cached = 0;
    bool have_cached = false;
    {
        auto &cache = app_info_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto it = cache.action_mask.find(title_id);
        if (it != cache.action_mask.end()) {
            cached = it->second;
            have_cached = true;
        }
    }

    uint32_t mask = cached;
    if (!have_cached) {
        const auto app = find_app_by_title_id(title_id);
        const bool has_app = app.has_value();

        if (has_app && !app->savedata.empty() && fs::exists(emuenv->vita_fs_path / "ux0/user" / emuenv->io.user_id / "savedata" / app->savedata))
            mask |= ACTION_DELETE_SAVE_DATA;

        if (fs::exists(emuenv->vita_fs_path / "ux0/patch" / title_id)
            || fs::exists(emuenv->shared_path / "patch" / title_id))
            mask |= ACTION_DELETE_PATCH;

        if (has_app && !app->addcont.empty() && fs::exists(emuenv->vita_fs_path / "ux0/addcont" / app->addcont))
            mask |= ACTION_DELETE_DLC;

        if (fs::exists(emuenv->vita_fs_path / "ux0/license" / title_id))
            mask |= ACTION_DELETE_LICENSE;

        if (fs::exists(emuenv->cache_path / "shaders" / title_id))
            mask |= ACTION_DELETE_SHADER_CACHE;

        if (fs::exists(emuenv->cache_path / "shaderlog" / title_id)
            || fs::exists(emuenv->log_path / "shaderlog" / title_id))
            mask |= ACTION_DELETE_SHADER_LOG;

        if (fs::exists(emuenv->shared_path / "textures/export" / title_id))
            mask |= ACTION_DELETE_EXPORT_TEXTURES;

        if (fs::exists(emuenv->shared_path / "textures/import" / title_id))
            mask |= ACTION_DELETE_IMPORT_TEXTURES;

        if (has_app)
            mask |= ACTION_RESET_LAST_PLAYED;
    }

    if (!session_active && find_app_by_title_id(title_id).has_value())
        mask |= ACTION_DELETE_APPLICATION;
    else
        mask &= ~ACTION_DELETE_APPLICATION;

    if (!have_cached) {
        // Stored without the session-dependent bit so a cached entry stays valid whether or
        // not a game happens to be running.
        auto &cache = app_info_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.action_mask[title_id] = mask & ~static_cast<uint32_t>(ACTION_DELETE_APPLICATION);
    }

    return mask;
}
