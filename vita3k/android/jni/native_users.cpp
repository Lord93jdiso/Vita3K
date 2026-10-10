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

#include <app/functions.h>
#include <config/functions.h>
#include <config/state.h>
#include <io/state.h>
#include <util/log.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

// Resolved once: getUsers is re-queried on every user screen refresh.
struct UsersJniCache {
    std::mutex mutex;
    jclass user_class = nullptr;
    jmethodID ctor = nullptr;
};

UsersJniCache &users_jni() {
    static UsersJniCache cache;
    return cache;
}

bool ensure_users_jni(JNIEnv *env) {
    auto &cache = users_jni();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.user_class)
            return true;
    }

    jclass local_class = env->FindClass("org/vita3k/emulator/data/NativeUser");
    if (!local_class)
        return false;

    jmethodID ctor = env->GetMethodID(local_class, "<init>", "(Ljava/lang/String;Ljava/lang/String;Z)V");
    if (!ctor) {
        env->DeleteLocalRef(local_class);
        return false;
    }

    jclass global_class = reinterpret_cast<jclass>(env->NewGlobalRef(local_class));
    env->DeleteLocalRef(local_class);

    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.user_class) {
        // Another thread won the race; keep its result and drop ours.
        if (global_class)
            env->DeleteGlobalRef(global_class);
        return true;
    }

    cache.user_class = global_class;
    cache.ctor = ctor;
    return cache.user_class != nullptr;
}

// User management rewrites io.user_id and the on-disk config, which live game threads
// read concurrently. Refuse while a session is live rather than corrupting its state.
bool refuse_if_session_active(const char *operation) {
    auto *controller = get_app_session_controller();
    if (controller && controller->has_active_session()) {
        LOG_WARN("Ignoring '{}' while an app session is active.", operation);
        return true;
    }
    return false;
}

// app::UserListState carries no mutex, so every read and mutation of the user map goes
// through this one. Without it, getUsers() iterating the map while createUser() inserts
// is iterator invalidation.
std::mutex &user_list_mutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

extern "C" {

JNIEXPORT jobjectArray JNICALL
Java_org_vita3k_emulator_NativeLib_getUsers(JNIEnv *env, jclass) {
    if (!ensure_users_jni(env))
        return nullptr;

    jclass user_class = nullptr;
    jmethodID ctor = nullptr;
    {
        auto &cache = users_jni();
        std::lock_guard<std::mutex> lock(cache.mutex);
        user_class = cache.user_class;
        ctor = cache.ctor;
    }

    auto *emuenv = get_emuenv();
    if (!emuenv)
        return env->NewObjectArray(0, user_class, nullptr);

    // UserListState has no mutex of its own, so the map is copied here while holding one.
    // Without it, a createUser/deleteUser arriving from another thread would be an
    // iterator invalidation while this loop walks the map.
    std::vector<std::pair<std::string, std::string>> users;
    {
        std::lock_guard<std::mutex> lock(user_list_mutex());
        users.reserve(emuenv->app.user_list.users.size());
        for (const auto &[id, user] : emuenv->app.user_list.users) {
            users.emplace_back(id, user.name);
        }
    }

    jobjectArray result = env->NewObjectArray(static_cast<jsize>(users.size()), user_class, nullptr);

    jsize index = 0;
    for (const auto &[id, user_name] : users) {
        jstring user_id = env->NewStringUTF(id.c_str());
        jstring name = env->NewStringUTF(user_name.c_str());
        const jboolean is_active = (id == emuenv->io.user_id) ? JNI_TRUE : JNI_FALSE;

        jobject user_obj = env->NewObject(user_class, ctor, user_id, name, is_active);
        env->SetObjectArrayElement(result, index++, user_obj);

        env->DeleteLocalRef(user_obj);
        env->DeleteLocalRef(user_id);
        env->DeleteLocalRef(name);
    }

    return result;
}

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_createUser(JNIEnv *env, jclass, jstring name_str) {
    auto *emuenv = get_emuenv();
    if (!emuenv || refuse_if_session_active("createUser"))
        return env->NewStringUTF("");

    const std::string name = jstring_to_string(env, name_str);
    if (name.empty())
        return env->NewStringUTF("");

    // Held across the create call and the config write, so the map and the active user
    // stay consistent with what a concurrent getUsers() observes.
    const std::lock_guard<std::mutex> lock(user_list_mutex());

    const std::string new_id = app::create_user(*emuenv, name);
    if (new_id.empty())
        return env->NewStringUTF("");

    if (emuenv->io.user_id.empty() && app::activate_user(*emuenv, new_id)) {
        emuenv->cfg.user_id = new_id;
        config::serialize_config(emuenv->cfg, emuenv->cfg.config_path);
    }

    return env->NewStringUTF(new_id.c_str());
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_activateUser(JNIEnv *env, jclass, jstring user_id_str) {
    auto *emuenv = get_emuenv();
    if (!emuenv || refuse_if_session_active("activateUser"))
        return JNI_FALSE;

    const std::string user_id = jstring_to_string(env, user_id_str);

    // Held across the activate call and the config write so the active user and the map
    // stay consistent with what a concurrent getUsers() observes.
    const std::lock_guard<std::mutex> lock(user_list_mutex());
    if (!app::activate_user(*emuenv, user_id))
        return JNI_FALSE;

    emuenv->cfg.user_id = user_id;
    config::serialize_config(emuenv->cfg, emuenv->cfg.config_path);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_deleteUser(JNIEnv *env, jclass, jstring user_id_str) {
    auto *emuenv = get_emuenv();
    if (!emuenv || refuse_if_session_active("deleteUser"))
        return JNI_FALSE;

    const std::string user_id = jstring_to_string(env, user_id_str);
    const std::lock_guard<std::mutex> lock(user_list_mutex());

    if (emuenv->app.user_list.users.find(user_id) == emuenv->app.user_list.users.end())
        return JNI_FALSE;

    app::delete_user(*emuenv, user_id);

    if (emuenv->io.user_id.empty() && !emuenv->app.user_list.users.empty()) {
        const auto first = emuenv->app.user_list.users.begin();
        if (!app::activate_user(*emuenv, first->first))
            return JNI_FALSE;

        emuenv->cfg.user_id = first->first;
        config::serialize_config(emuenv->cfg, emuenv->cfg.config_path);
    }

    return JNI_TRUE;
}

} // extern "C"
