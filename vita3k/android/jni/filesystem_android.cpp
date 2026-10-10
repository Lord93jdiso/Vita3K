// Vita3K emulator project
// Copyright (C) 2025 Vita3K team
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

#include <util/log.h>

#include <SDL3/SDL_system.h>
#include <SDL3/SDL_timer.h>

#include <chrono>
#include <condition_variable>
#include <jni.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <util/fs.h>

namespace host::dialog::filesystem {

enum Result {
    ERROR,
    SUCCESS,
    CANCEL,
};

struct FileFilter {
    std::string display_name = "";
    std::vector<std::string> file_extensions = {};
};

Result open_file(fs::path &resulting_path, const std::vector<FileFilter> &file_filters = {}, const fs::path &default_path = "");
Result pick_folder(fs::path &resulting_path, const fs::path &default_path = "");
std::string get_error();

} // namespace host::dialog::filesystem

namespace {

// The guest thread blocks until the Java side delivers a result through
// filedialogReturn(). A plain flag plus a polling sleep would either spin the CPU or - if
// the result never arrives - hang the guest thread forever, so the wait is a condition
// variable with a timeout instead.
struct DialogWaitState {
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    fs::path result;
};

constexpr auto dialog_timeout = std::chrono::minutes(5);

DialogWaitState &dialog_wait_state() {
    static DialogWaitState state;
    return state;
}

// Takes the pending result out of the shared state, if there is one.
fs::path take_dialog_result() {
    auto &state = dialog_wait_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.result.empty())
        return fs::path();

    fs::path result = std::move(state.result);
    state.result.clear();
    return result;
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_org_vita3k_emulator_Emulator_filedialogReturn(JNIEnv *env, jobject thiz, jstring result_path) {
    auto &state = dialog_wait_state();

    fs::path path;
    if (result_path) {
        const char *result_ptr = env->GetStringUTFChars(result_path, nullptr);
        if (result_ptr) {
            path = fs::path(result_ptr);
            env->ReleaseStringUTFChars(result_path, result_ptr);
        }
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.result = std::move(path);
        state.completed = true;
    }
    state.cv.notify_all();
}

static bool call_dialog_java_function(const char *name, bool need_write) {
    (void)need_write;

    JNIEnv *env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return false;

    jclass clazz(env->GetObjectClass(activity));
    if (!clazz) {
        LOG_ERROR("Failed to resolve the activity class while opening the '{}' dialog.", name);
        return false;
    }

    jmethodID method_id = env->GetMethodID(clazz, name, "()V");
    if (!method_id) {
        LOG_ERROR("Activity has no '{}' method, cannot open the dialog.", name);
        env->DeleteLocalRef(clazz);
        return false;
    }

    auto &state = dialog_wait_state();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.result.clear();
        state.completed = false;
    }

    env->CallVoidMethod(activity, method_id);

    env->DeleteLocalRef(clazz);

    std::unique_lock<std::mutex> lock(state.mutex);
    const bool completed = state.cv.wait_for(lock, dialog_timeout, [&state] {
        return state.completed;
    });
    if (!completed) {
        state.completed = false;
        state.result.clear();
        LOG_ERROR("Timed out waiting for the '{}' dialog result, continuing without a selection.", name);
        return false;
    }

    state.completed = false;
    return true;
}

namespace host::dialog::filesystem {

Result open_file(fs::path &resulting_path, const std::vector<FileFilter> &file_filters, const fs::path &default_path) {
    call_dialog_java_function("showFileDialog", false);

    const fs::path selected = take_dialog_result();
    if (selected.empty())
        return Result::CANCEL;

    resulting_path = selected;
    return Result::SUCCESS;
}

Result pick_folder(fs::path &resulting_path, const fs::path &default_path) {
    call_dialog_java_function("showFolderDialog", true);

    const fs::path selected = take_dialog_result();
    if (selected.empty())
        return Result::CANCEL;

    resulting_path = selected;
    return Result::SUCCESS;
}

std::string get_error() {
    return "";
}

} // namespace host::dialog::filesystem
