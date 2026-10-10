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
#include "interface.h"

#include <app/functions.h>
#include <audio/state.h>
#include <config/settings.h>
#include <ctrl/functions.h>
#include <dialog/state.h>
#include <ime/functions.h>
#include <ime/keyboard.h>
#include <io/state.h>
#include <motion/event_handler.h>
#include <motion/functions.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <renderer/vulkan/screen_renderer.h>
#include <touch/functions.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <jni.h>
#include <mutex>
#include <optional>
#include <string>

namespace {

// Floor on how fast the event loop may iterate when the queue is never empty. Comfortably
// above any real event rate, so no input is delayed, but far below spinning at core speed.
constexpr long long event_loop_min_iteration_us = 2000;

void reset_android_session_audio(EmuEnvState &emuenv) {
    if (!emuenv.audio.adapter)
        return;

    LOG_DEBUG("Resetting Android audio backend '{}' after SDL session shutdown", emuenv.audio.audio_backend);
    emuenv.audio.adapter.reset();
    emuenv.audio.audio_backend.clear();
}

bool handle_ime_keydown(EmuEnvState &emuenv, const SDL_KeyboardEvent &event) {
    if (!is_any_ime_active(emuenv))
        return false;

    auto &ime = emuenv.ime;
    const bool dialog_ime_active = is_ime_dialog_active(emuenv);

    switch (event.key) {
    case SDLK_BACKSPACE: {
        {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime_backspace(ime);
        }
        ime::notify_ime_state_changed();
        return true;
    }

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (dialog_ime_active) {
            finish_ime_dialog(emuenv);
        } else {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime.event_id = SCE_IME_EVENT_PRESS_ENTER;
        }
        ime::notify_ime_state_changed();
        return true;

    case SDLK_ESCAPE:
        if (dialog_ime_active) {
            cancel_ime_dialog(emuenv);
        } else {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime.event_id = SCE_IME_EVENT_PRESS_CLOSE;
        }
        ime::notify_ime_state_changed();
        return true;

    case SDLK_LEFT: {
        {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime_cursor_left(ime);
        }
        ime::notify_ime_state_changed();
        return true;
    }

    case SDLK_RIGHT: {
        {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime_cursor_right(ime);
        }
        ime::notify_ime_state_changed();
        return true;
    }

    default:
        return false;
    }
}

void handle_ime_text_editing(EmuEnvState &emuenv, const char *text) {
    if (!is_any_ime_active(emuenv))
        return;

    {
        std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        ime_set_preedit(emuenv.ime, string_utils::utf8_to_utf16(text ? text : ""));
    }
    ime::notify_ime_state_changed();
}

void handle_ime_text_input(EmuEnvState &emuenv, const char *text) {
    if (!is_any_ime_active(emuenv) || !text || text[0] == '\0')
        return;

    std::string filtered_text;
    filtered_text.reserve(std::strlen(text));
    for (const char *ch = text; *ch != '\0'; ++ch) {
        if (*ch != '\n' && *ch != '\r')
            filtered_text.push_back(*ch);
    }

    if (filtered_text.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        ime_commit_text(emuenv.ime, string_utils::utf8_to_utf16(filtered_text));
    }
    ime::notify_ime_state_changed();
}

class AndroidFrameHost final : public renderer::FrameHost {
public:
    explicit AndroidFrameHost(SDL_Window *window, SDL_GLContext *gl_context)
        : m_window(window)
        , m_gl_context(gl_context) {
    }

    renderer::DisplayHandle handle() const override {
        return renderer::AndroidDisplayHandle{ m_window };
    }

    int drawable_width() const override {
#ifdef __ANDROID__
        if (!renderer::vulkan::has_android_surface())
            return 0;
#endif
        int width = 960;
        int height = 544;
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
        return width;
    }

    int drawable_height() const override {
#ifdef __ANDROID__
        if (!renderer::vulkan::has_android_surface())
            return 0;
#endif
        int width = 960;
        int height = 544;
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
        return height;
    }

    std::vector<std::string> font_dirs() const override {
        return { "/system/fonts/" };
    }

    void *get_proc_address(const char *name) const override {
        return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name));
    }

    unsigned int default_fbo() const override {
        return 0;
    }

    bool make_current() override {
        if (!m_gl_context || !*m_gl_context)
            return false;
        return SDL_GL_MakeCurrent(m_window, *m_gl_context);
    }

    void done_current() override {
        SDL_GL_MakeCurrent(m_window, nullptr);
    }

    void swap_buffers() override {
        SDL_GL_SwapWindow(m_window);
    }

    bool set_vsync(bool enabled) override {
        return SDL_GL_SetSwapInterval(enabled ? 1 : 0);
    }

    void prepare_for_render_thread() override {
        if (m_gl_context && *m_gl_context)
            SDL_GL_MakeCurrent(m_window, nullptr);
    }

    void destroy_render_context() override {
        if (!m_gl_context || !*m_gl_context)
            return;

        SDL_GL_DestroyContext(*m_gl_context);
        *m_gl_context = nullptr;
    }

private:
    SDL_Window *m_window = nullptr;
    SDL_GLContext *m_gl_context = nullptr;
};

} // namespace

// JNI handles resolved once per process: the SDL activity reference is owned by SDL, so it
// must never be passed to DeleteLocalRef, and the string-based method lookups it replaces ran
// on every call. Defined at file scope because C++ does not allow a function definition
// inside another one, which is where this first landed.
struct AndroidActivityJni {
    std::mutex mutex;
    jclass clazz = nullptr;
    jmethodID open_pause_menu = nullptr;
    jmethodID set_current_game_id = nullptr;
    jmethodID get_display_rotation = nullptr;
};

AndroidActivityJni &activity_jni() {
    static AndroidActivityJni cache;
    return cache;
}

bool ensure_activity_jni(JNIEnv *jni_env, jobject activity) {
    auto &cache = activity_jni();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.clazz)
            return true;
    }

    jclass local_clazz = jni_env->GetObjectClass(activity);
    if (!local_clazz)
        return false;

    jmethodID open_pause_menu = jni_env->GetMethodID(local_clazz, "openPauseMenuFromController", "()V");
    jmethodID set_current_game_id = jni_env->GetMethodID(local_clazz, "setCurrentGameId", "(Ljava/lang/String;)V");
    jmethodID get_display_rotation = jni_env->GetMethodID(local_clazz, "getNativeDisplayRotation", "()I");

    // A global ref survives across threads; it is released again when the activity is
    // destroyed, otherwise every session restart would leak one.
    jclass global_clazz = reinterpret_cast<jclass>(jni_env->NewGlobalRef(local_clazz));
    jni_env->DeleteLocalRef(local_clazz);

    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.clazz) {
        // Another thread resolved it first; keep its result and drop ours.
        if (global_clazz)
            jni_env->DeleteGlobalRef(global_clazz);
        return true;
    }

    cache.open_pause_menu = open_pause_menu;
    cache.set_current_game_id = set_current_game_id;
    cache.get_display_rotation = get_display_rotation;
    cache.clazz = global_clazz;

    return cache.clazz != nullptr;
}

void invalidate_activity_jni() {
    auto &cache = activity_jni();
    jclass stale = nullptr;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        stale = cache.clazz;
        cache.clazz = nullptr;
        cache.open_pause_menu = nullptr;
        cache.set_current_game_id = nullptr;
        cache.get_display_rotation = nullptr;
    }

    JNIEnv *jni_env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    if (jni_env && stale)
        jni_env->DeleteGlobalRef(stale);
}

extern "C" {

// SDL3 entry point — called by SDLActivity's native thread when the Emulator activity starts.
// argv is populated from Emulator.getArguments(), e.g. {"-r", "PCSE00000"}.
SDLMAIN_DECLSPEC int SDL_main(int argc, char *argv[]) {
    std::string title_id;
    for (int i = 0; i < argc; i++) {
        if (std::string(argv[i]) == "-r" && i + 1 < argc) {
            title_id = argv[i + 1];
            break;
        }
    }

    if (title_id.empty()) {
        LOG_ERROR("No title ID provided");
        return -1;
    }

    auto *emuenv = get_emuenv();
    auto *session_controller = get_app_session_controller();
    if (!emuenv) {
        LOG_ERROR("Emulator not initialized");
        return -1;
    }
    if (!session_controller) {
        LOG_ERROR("App session controller is unavailable");
        return -1;
    }

    const auto open_pause_menu = []() {
        JNIEnv *jni_env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
        jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
        if (!jni_env || !activity)
            return;

        if (!ensure_activity_jni(jni_env, activity))
            return;

        jmethodID method_id = nullptr;
        {
            auto &cache = activity_jni();
            std::lock_guard<std::mutex> lock(cache.mutex);
            method_id = cache.open_pause_menu;
        }

        if (method_id)
            jni_env->CallVoidMethod(activity, method_id);
    };

    const auto sync_current_game_id = [](const std::string &game_id) {
        JNIEnv *jni_env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
        jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
        if (!jni_env || !activity)
            return;

        if (!ensure_activity_jni(jni_env, activity))
            return;

        jmethodID method_id = nullptr;
        {
            auto &cache = activity_jni();
            std::lock_guard<std::mutex> lock(cache.mutex);
            method_id = cache.set_current_game_id;
        }

        if (method_id) {
            jstring jgame_id = jni_env->NewStringUTF(game_id.c_str());
            jni_env->CallVoidMethod(activity, method_id, jgame_id);
            jni_env->DeleteLocalRef(jgame_id);
        }
    };

    AppLaunchRequest launch_request{
        .app_path = title_id,
    };
    int exit_code = 0;
    bool relaunch_requested = false;

    do {
        relaunch_requested = false;

        SDL_Window *window = nullptr;
        SDL_GLContext gl_context = nullptr;
        AndroidFrameHost frame_host(nullptr, &gl_context);
        std::optional<AppLaunchRequest> pending_launch_request;

        const auto cleanup_launch = [&](const app::AppSessionStopReason reason) {
            detach_overlay_virtual_controller();
            session_controller->stop(reason);
            reset_android_session_audio(*emuenv);

            ime::set_sdl_window(nullptr);
            // Cached activity references belong to the activity that is going away.
            ime::on_activity_recreated();
            invalidate_activity_jni();
            if (window) {
                SDL_DestroyWindow(window);
                window = nullptr;
            }

            SDL_Quit();
        };

        LOG_INFO("Booting game '{}'", launch_request.app_path);

        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_SENSOR | SDL_INIT_CAMERA)) {
            LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        refresh_controllers(emuenv->ctrl, *emuenv);

        {
            JNIEnv *jni_env = reinterpret_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
            jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
            if (jni_env && activity && ensure_activity_jni(jni_env, activity)) {
                jmethodID method_id = nullptr;
                {
                    auto &cache = activity_jni();
                    std::lock_guard<std::mutex> lock(cache.mutex);
                    method_id = cache.get_display_rotation;
                }

                if (method_id)
                    set_display_rotation(emuenv->motion, jni_env->CallIntMethod(activity, method_id));
            }
        }

        if (!session_controller->begin_launch(launch_request, launch_request.reason != AppLaunchReason::LoadExec)) {
            LOG_ERROR("Could not find app '{}' in apps list.", launch_request.app_path);
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }
        sync_current_game_id(launch_request.app_path);

        SDL_WindowFlags window_flags = 0;
        if (emuenv->backend_renderer == renderer::Backend::OpenGL) {
            window_flags |= SDL_WINDOW_OPENGL;
            if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES)
                || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3)
                || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2)
                || !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)
                || !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0)) {
                LOG_ERROR("Failed to configure OpenGL ES context attributes: {}", SDL_GetError());
                exit_code = -1;
                cleanup_launch(app::AppSessionStopReason::LaunchFailure);
                break;
            }
#ifndef NDEBUG
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
        } else if (emuenv->backend_renderer == renderer::Backend::Vulkan) {
            window_flags |= SDL_WINDOW_VULKAN;
        }

        SDL_PropertiesID window_props = SDL_CreateProperties();
        if (!window_props) {
            LOG_ERROR("SDL_CreateProperties failed: {}", SDL_GetError());
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        SDL_SetStringProperty(window_props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Vita3K");
        SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 960);
        SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 544);
        SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, window_flags);
        if (emuenv->backend_renderer == renderer::Backend::OpenGL)
            SDL_SetBooleanProperty(window_props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);

        window = SDL_CreateWindowWithProperties(window_props);
        SDL_DestroyProperties(window_props);
        if (!window) {
            LOG_ERROR("SDL_CreateWindowWithProperties failed: {}", SDL_GetError());
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }
        ime::set_sdl_window(window);
        frame_host = AndroidFrameHost(window, &gl_context);

        if (emuenv->backend_renderer == renderer::Backend::OpenGL) {
            gl_context = SDL_GL_CreateContext(window);
            if (!gl_context) {
                LOG_ERROR("Failed to create OpenGL ES context: {}", SDL_GetError());
                exit_code = -1;
                cleanup_launch(app::AppSessionStopReason::LaunchFailure);
                break;
            }

            if (!SDL_GL_MakeCurrent(window, gl_context)) {
                LOG_ERROR("Failed to make OpenGL ES context current: {}", SDL_GetError());
                exit_code = -1;
                cleanup_launch(app::AppSessionStopReason::LaunchFailure);
                break;
            }

            if (!SDL_GL_SetSwapInterval(static_cast<int>(emuenv->cfg.current_config.v_sync)))
                LOG_WARN("Failed to set OpenGL ES swap interval: {}", SDL_GetError());
        }

        if (!session_controller->initialize_renderer(frame_host)) {
            LOG_ERROR("Failed to initialise renderer.");
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        if (!session_controller->initialize_runtime()) {
            LOG_ERROR("Failed late initialisation.");
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        if (!session_controller->load_and_run()) {
            LOG_ERROR("Failed to load or start the app session.");
            exit_code = -1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        if (auto request = emuenv->take_app_launch_request())
            pending_launch_request = std::move(request);

        LOG_INFO("Game started: {} ({})", emuenv->current_app_title, launch_request.app_path);
        app::LaunchRuntimeMetrics runtime_metrics{};

        bool running = !pending_launch_request.has_value();

        // Shared by the drain loop and the single event returned from
        // SDL_WaitEventTimeout, so a woken event is handled identically to a polled one.
        const auto handle_event = [&](SDL_Event &event) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                break;

            case SDL_EVENT_KEY_DOWN:
                if (handle_ime_keydown(*emuenv, event.key))
                    break;
                break;

            case SDL_EVENT_TEXT_EDITING:
                handle_ime_text_editing(*emuenv, event.edit.text);
                break;

            case SDL_EVENT_TEXT_INPUT:
                handle_ime_text_input(*emuenv, event.text.text);
                break;

            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED: {
                handle_touch_event(emuenv->touch, event.tfinger);
                auto &mouse = emuenv->ctrl.overlay_mouse;
                mouse.x.store(event.tfinger.x * 960.f, std::memory_order_relaxed);
                mouse.y.store(event.tfinger.y * 544.f, std::memory_order_relaxed);
                mouse.pressed.store(event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION, std::memory_order_relaxed);
                break;
            }

            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                refresh_controllers(emuenv->ctrl, *emuenv);
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
                    open_pause_menu();
                break;

            case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
            case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
            case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
                handle_touchpad_event(emuenv->touch, event.gtouchpad);
                break;

            case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
                handle_motion_event(*emuenv, event.gsensor.sensor, event.gsensor);
                break;
            case SDL_EVENT_SENSOR_UPDATE:
                handle_motion_event(*emuenv, SDL_GetSensorTypeForID(event.sensor.which), event.sensor);
                break;

            default:
                break;
            }
        };

        while (running) {
            SDL_Event event;
            bool handled_event = false;
            const auto iteration_start = std::chrono::steady_clock::now();

            // Drain whatever is already queued.
            while (SDL_PollEvent(&event)) {
                handled_event = true;
                handle_event(event);
            }

            if (!pending_launch_request) {
                if (auto request = emuenv->take_app_launch_request()) {
                    pending_launch_request = std::move(request);
                    running = false;
                }
            }

            app::update_runtime_metrics(*emuenv, runtime_metrics);

            if (!session_controller->is_running())
                running = false;

            if (!running)
                break;

            // A fixed 16 ms sleep caps polling at 60 Hz and adds up to a full frame of
            // latency to every input event. Waiting on the event queue instead wakes the
            // loop the instant something arrives and only falls back to the timeout when
            // the guest is idle.
            if (!handled_event) {
                if (SDL_WaitEventTimeout(&event, 16))
                    handle_event(event);
                continue;
            }

            // Events arrived, so the loop never sleeps. High-rate sources - finger motion
            // on the overlay, gamepad sensors - keep the queue permanently non-empty, and
            // spinning through it would burn a core for nothing. The budget still covers a
            // frame several times over, so input stays well under one frame of latency.
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - iteration_start);
            if (elapsed.count() < event_loop_min_iteration_us) {
                const auto remaining = std::chrono::microseconds(event_loop_min_iteration_us) - elapsed;
                SDL_Delay(static_cast<Uint32>(remaining.count() / 1000));
            }
        }

        LOG_INFO("Shutting down game");

        if (pending_launch_request) {
            launch_request = std::move(*pending_launch_request);
            relaunch_requested = true;
            LOG_INFO("Relaunching in-process with self '{}'", launch_request.self_path);
        }

        cleanup_launch(relaunch_requested
                ? app::AppSessionStopReason::Relaunch
                : app::AppSessionStopReason::UserRequest);
    } while (relaunch_requested);

    return exit_code;
}

} // extern "C"
