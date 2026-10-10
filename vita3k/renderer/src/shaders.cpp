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

#include <renderer/shaders.h>

#include <renderer/vulkan/state.h>

#include <gxm/types.h>
#include <renderer/state.h>
#include <renderer/types.h>
#include <shader/spirv_recompiler.h>
#include <util/fs.h>
#include <util/log.h>

#include <string>
#include <vector>

namespace renderer {

bool get_shaders_cache_hashs(State &renderer) {
    const std::string hash_file_name = fmt::format("hashs-{}.dat", (renderer.current_backend == Backend::OpenGL) ? "gl" : "vk");

    fs::ifstream shaders_hashs(renderer.shaders_path / hash_file_name, std::ios::in | std::ios::binary);
    if (!shaders_hashs.is_open())
        return false;

    // A truncated file leaves the fields below uninitialised and the entry count garbage,
    // which on Android is a realistic outcome (the process is killed mid-write, or the
    // volume fills up). Everything is therefore read through a checked helper and a bad
    // file is treated as a cache miss instead of being trusted.
    const auto read_or_fail = [&shaders_hashs](auto &value) {
        shaders_hashs.read(reinterpret_cast<char *>(&value), sizeof(value));
        return shaders_hashs.gcount() == static_cast<std::streamsize>(sizeof(value));
    };

    const auto discard_cache = [&renderer, &hash_file_name, &shaders_hashs](const char *reason) {
        LOG_WARN("Shader cache '{}' is unusable ({}), recreating it.", hash_file_name, reason);
        // Closed first: on Windows the open handle locks the file and remove_all would
        // silently fail, leaving the corrupt cache in place.
        shaders_hashs.close();
        renderer.shaders_cache_hashs.clear();
        fs::remove_all(renderer.shaders_path);
        fs::remove_all(renderer.shaders_log_path);
    };

    renderer.shaders_cache_hashs.clear();

    size_t size = 0;
    uint32_t versionInFile = 0;
    uint32_t features_mask = 0;
    if (!read_or_fail(size) || !read_or_fail(versionInFile) || !read_or_fail(features_mask)) {
        discard_cache("truncated header");
        return false;
    }

    if (versionInFile != shader::CURRENT_VERSION || features_mask != renderer.get_features_mask()) {
        discard_cache(versionInFile != shader::CURRENT_VERSION ? "outdated version" : "incompatible GPU features");

        if (versionInFile != shader::CURRENT_VERSION)
            LOG_WARN("Current version of cache: {}, is outdated, recreate it.", versionInFile);
        else
            LOG_WARN("Incompatible GPU features enabled, recreating shader cache");
        return false;
    }

    if (renderer.current_backend == Backend::Vulkan) {
        // Read the pipeline cache
        dynamic_cast<vulkan::VKState &>(renderer).pipeline_cache.read_pipeline_cache();
    }

    // Clamp against what the file can actually hold, so a corrupt count cannot be turned
    // into a huge allocation.
    constexpr uint64_t entry_size = sizeof(Sha256Hash) * 2;
    constexpr uint64_t header_size = sizeof(size_t) + sizeof(uint32_t) * 2;

    shaders_hashs.seekg(0, std::ios::end);
    const std::streampos end_pos = shaders_hashs.tellg();
    // Rewound to where the header read left off, not to zero: the entry list starts right
    // after the header, and seeking to 0 would shift every hash by header_size bytes so no
    // entry would ever match - the cache would silently miss on every single launch.
    shaders_hashs.seekg(static_cast<std::streamoff>(header_size), std::ios::beg);
    if (end_pos < 0) {
        discard_cache("could not determine file size");
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(end_pos);

    // A cache larger than this is corrupt; each entry is only 64 bytes of hash.
    constexpr uint64_t max_cache_size = 32u * 1024u * 1024u;
    if (file_size < header_size || file_size > max_cache_size) {
        discard_cache("implausible file size");
        return false;
    }

    const uint64_t max_entries = (file_size - header_size) / entry_size;
    if (static_cast<uint64_t>(size) > max_entries) {
        discard_cache("entry count does not match file size");
        return false;
    }

    // Read Hashs info value
    renderer.shaders_cache_hashs.reserve(size);
    for (size_t a = 0; a < size; a++) {
        ShadersHash hash{};
        if (!read_or_fail(hash.frag) || !read_or_fail(hash.vert)) {
            discard_cache("truncated hash list");
            return false;
        }

        renderer.shaders_cache_hashs.push_back(hash);
    }

    shaders_hashs.close();

    return !renderer.shaders_cache_hashs.empty();
}

void save_shaders_cache_hashs(State &renderer, std::vector<ShadersHash> &shaders_cache_hashs) {
    fs::create_directories(renderer.shaders_path);
    std::string hash_file_name = fmt::format("hashs-{}.dat", (renderer.current_backend == Backend::OpenGL) ? "gl" : "vk");
    fs::ofstream shaders_hashs(renderer.shaders_path / hash_file_name, std::ios::out | std::ios::binary);

    if (shaders_hashs.is_open()) {
        // Write Size of shaders cache hashes list
        const auto size = shaders_cache_hashs.size();
        shaders_hashs.write((const char *)&size, sizeof(size));

        // Write version of cache
        const uint32_t versionInFile = shader::CURRENT_VERSION;
        shaders_hashs.write((const char *)&versionInFile, sizeof(uint32_t));
        const uint32_t features_mask = renderer.get_features_mask();
        shaders_hashs.write((const char *)&features_mask, sizeof(uint32_t));

        // Write shader hash list
        for (const auto &hash : shaders_cache_hashs) {
            auto write = [&shaders_hashs](const Sha256Hash &hash) {
                shaders_hashs.write(reinterpret_cast<const char *>(hash.data()), sizeof(Sha256Hash));
            };

            write(hash.frag);
            write(hash.vert);
        }
        shaders_hashs.close();
    }
}

static bool load_shader(const fs::path &shader_name, char **destination, std::size_t &size_read) {
    fs::ifstream is(shader_name, fs::ifstream::binary);
    if (!is) {
        return false;
    }

    is.seekg(0, fs::ifstream::end);
    size_read = is.tellg();
    is.seekg(0);

    if (size_read == 0) {
        return false;
    }

    if (destination == nullptr) {
        return true;
    }

    is.read(*destination, size_read);
    return true;
}

static Sha256Hash get_shader_hash(const SceGxmProgram &program) {
    const Sha256Hash hash_bytes = sha256(&program, program.size);
    return hash_bytes;
}

template <typename R>
static R load_shader_generic(const fs::path &shader_path) {
    std::size_t read_size = 0;
    R source;

    if (load_shader(shader_path, nullptr, read_size)) {
        source.resize((read_size + sizeof(typename R::value_type) - 1) / sizeof(typename R::value_type));

        char *dest_pointer = reinterpret_cast<char *>(source.data());
        load_shader(shader_path, &dest_pointer, read_size);
    }

    return source;
}

static shader::GeneratedShader load_shader_generic(shader::Target target, const SceGxmProgram &program, const FeatureState &features, const shader::Hints &hints, bool maskupdate, const fs::path &shader_cache_path, const fs::path &shaderlog_path, const char *shader_type_str, const std::string &shader_version, bool shader_cache, bool dump_shader_log) {
    // TODO: no need to recompute the hash here
    const std::string hash_text = hex_string(get_shader_hash(program));
    // Set Shader Hash with Version
    const std::string hash_hex_ver = fmt::format("{}-{}", shader_version, hash_text);
    const auto get_shader_path = [&](const char *ext) {
        return shader_cache_path / fmt::format("{}.{}", hash_hex_ver, ext);
    };
    const auto get_shaderlog_path = [&](const char *ext) {
        return shaderlog_path / fmt::format("{}.{}", hash_hex_ver, ext);
    };

    const auto shader_path = get_shader_path(shader_type_str);
    if (shader_cache) {
        if (target == shader::Target::GLSLOpenGL) {
            std::string source = load_shader_generic<std::string>(shader_path);
            if (!source.empty()) {
                return { source, std::vector<uint32_t>() };
            }
        } else {
            std::vector<uint32_t> source = load_shader_generic<std::vector<uint32_t>>(get_shader_path("spv"));
            if (!source.empty())
                return { "", source };
        }
    }

    LOG_INFO("Generating {} shader {}", shader_type_str, hash_text);

    // Every newly generated shader used to be dumped unconditionally as .gxp plus a
    // disassembly and the GLSL source. On Android that is several megabytes per compile
    // written to app-external storage, which is often FUSE-backed, and there was no way
    // to turn it off. The cache itself is still written when shader_cache is enabled.
    const bool write_shader_cache = shader_cache;

    const auto write_data_with_ext = [&](const std::string &ext, const std::string &data) {
        // On the GLSL path the generated source for the active type is the cache entry.
        // The SPIR-V path has no such dumper call (its cache entry is written below), so
        // there "frag"/"vert" is diagnostic output.
        if (ext == shader_type_str && target == shader::Target::GLSLOpenGL && write_shader_cache) {
            fs_utils::dump_data(shader_path, data.c_str(), data.size());
            return true;
        }

        if (!dump_shader_log)
            return true;

        fs::create_directories(shaderlog_path);
        // The dumper hands over short names ("dsm", "spv", "frag", "vert"); keep them as
        // they are so a shader does not spread over one file per representation.
        fs_utils::dump_data(get_shaderlog_path(ext), data.c_str(), data.size());
        return true;
    };

    if (dump_shader_log) {
        fs::create_directories(shaderlog_path);
        // Dump gxp binary
        fs_utils::dump_data(get_shaderlog_path("gxp"), &program, program.size);
    }

    shader::GeneratedShader source = shader::convert_gxp(program, hash_text, features, target, hints, maskupdate, false, write_data_with_ext);

    // Copy shader generate to shaders cache
    const auto shaders_cache_path = fs::path(shader_cache_path);

    if (write_shader_cache) {
        fs::create_directories(shaders_cache_path);

        if (target != shader::Target::GLSLOpenGL) {
            const auto shader_dst_path = get_shader_path("spv");
            fs_utils::dump_data(shader_dst_path, source.spirv.data(), sizeof(uint32_t) * source.spirv.size());
        }
    }

    return source;
}

std::string load_glsl_shader(const SceGxmProgram &program, const FeatureState &features, const shader::Hints &hints, bool maskupdate, const fs::path &shader_cache_path, const fs::path &shader_log_path, const std::string &shader_version, bool shader_cache, bool dump_shader_log) {
    SceGxmProgramType program_type = program.get_type();

    auto shader_type_to_str = [](SceGxmProgramType type) {
        return (type == SceGxmProgramType::Vertex) ? "vert" : ((type == SceGxmProgramType::Fragment) ? "frag" : "unknown");
    };

    const char *shader_type_str = shader_type_to_str(program_type);

    return load_shader_generic(shader::Target::GLSLOpenGL, program, features, hints, maskupdate, shader_cache_path, shader_log_path, shader_type_str, shader_version, shader_cache, dump_shader_log).glsl;
}

std::vector<uint32_t> load_spirv_shader(const SceGxmProgram &program, const FeatureState &features, bool is_vulkan, const shader::Hints &hints, bool maskupdate, const fs::path &shader_cache_path, const fs::path &shader_log_path, const std::string &shader_version, bool shader_cache, bool dump_shader_log) {
    const shader::Target target = is_vulkan ? shader::Target::SpirVVulkan : shader::Target::SpirVOpenGL;
    auto shader_type_to_str = [](SceGxmProgramType type) {
        return (type == SceGxmProgramType::Vertex) ? "vert.spv.txt" : ((type == SceGxmProgramType::Fragment) ? "frag.spv.txt" : "unknown.spv.txt");
    };
    const char *shader_type_str = shader_type_to_str(program.get_type());

    return load_shader_generic(target, program, features, hints, maskupdate, shader_cache_path, shader_log_path, shader_type_str, shader_version, shader_cache, dump_shader_log).spirv;
}

std::string pre_load_shader_glsl(const fs::path &shader_path) {
    return load_shader_generic<std::string>(shader_path);
}

std::vector<uint32_t> pre_load_shader_spirv(const fs::path &shader_path) {
    return load_shader_generic<std::vector<uint32_t>>(shader_path);
}

} // namespace renderer
