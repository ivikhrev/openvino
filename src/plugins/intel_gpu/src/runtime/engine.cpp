// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "intel_gpu/runtime/engine.hpp"
#include "intel_gpu/runtime/event.hpp"
#include "intel_gpu/runtime/memory.hpp"
#include "intel_gpu/runtime/stream.hpp"
#include "intel_gpu/runtime/device_query.hpp"
#include "intel_gpu/runtime/debug_configuration.hpp"

#include "ocl/ocl_engine_factory.hpp"

#include <string>
#include <vector>
#include <memory>
#include <set>
#include <stdexcept>
#include <algorithm>

#if defined(_WIN32)
#include <windows.h>

static size_t get_cpu_ram_size() {
    MEMORYSTATUSEX s {};
    s.dwLength = sizeof(s);
    GlobalMemoryStatusEx(&s);
    return s.ullTotalPhys;
}
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__QNXNTO__)
#include <unistd.h>
#include <sys/sysctl.h>

static size_t get_cpu_ram_size() {
#ifdef __APPLE__
    int query_ram[] = {CTL_HW, HW_MEMSIZE};
#else
    int query_ram[] = {CTL_HW, HW_PHYSMEM};
#endif
    int query_ram_len = sizeof(query_ram) / sizeof(*query_ram);
    size_t totalram = 0;
    size_t length = sizeof(totalram);

    sysctl(query_ram, query_ram_len, &totalram, &length, NULL, 0);
    return totalram;
}
#else
#include <sys/sysinfo.h>

static size_t get_cpu_ram_size() {
    struct sysinfo s {};
    sysinfo(&s);
    return s.totalram;
}
#endif

namespace cldnn {

engine::engine(const device::ptr device)
    : _device(device) {}

device_info engine::get_device_info() const {
    return _device->get_info();
}

const device::ptr engine::get_device() const {
    return _device;
}

bool engine::use_unified_shared_memory() const {
    GPU_DEBUG_GET_INSTANCE(debug_config);
    GPU_DEBUG_IF(debug_config->disable_usm) {
        return false;
    }
    if (_device->get_mem_caps().supports_usm()) {
        return true;
    }
    return false;
}

uint64_t engine::get_max_memory_size() const {
    static uint64_t max_device_mem = (std::max)(get_device_info().max_global_mem_size, static_cast<uint64_t>(get_cpu_ram_size()));
    return max_device_mem;
}

bool engine::supports_allocation(allocation_type type) const {
    if (memory_capabilities::is_usm_type(type) && !use_unified_shared_memory())
        return false;
    if (allocation_type::usm_shared == type)
        return false;
    return _device->get_mem_caps().support_allocation_type(type);
}

allocation_type engine::get_lockable_preferred_memory_allocation_type(bool is_image_layout) const {
    if (!use_unified_shared_memory() || is_image_layout)
        return get_default_allocation_type();

    /*
        We do not check device allocation here.
        Device allocation is reserved for buffers of hidden layers.
        Const buffers are propagated to device if possible.
    */

    bool support_usm_host = supports_allocation(allocation_type::usm_host);
    bool support_usm_shared = supports_allocation(allocation_type::usm_shared);

    if (support_usm_shared)
        return allocation_type::usm_shared;
    if (support_usm_host)
        return allocation_type::usm_host;

    OPENVINO_ASSERT(false, "[GPU] Couldn't find proper allocation type in get_lockable_preferred_memory_allocation_type method");
}

allocation_type engine::get_preferred_memory_allocation_type(bool is_image_layout) const {
    if (!use_unified_shared_memory() || is_image_layout)
        return get_default_allocation_type();

    if (supports_allocation(allocation_type::usm_device))
        return allocation_type::usm_device;

    // Fallback to host allocations in case if device ones are not supported for some reason
    if (supports_allocation(allocation_type::usm_host))
        return allocation_type::usm_host;

    OPENVINO_ASSERT(false, "[GPU] Couldn't find proper allocation type in get_preferred_memory_allocation_type method");
}

memory::ptr engine::attach_memory(const layout& layout, void* ptr) {
    return std::make_shared<simple_attached_memory>(layout, ptr);
}

memory::ptr engine::allocate_memory(const layout& layout, bool reset) {
    allocation_type type = get_lockable_preferred_memory_allocation_type(layout.format.is_image_2d());
    return allocate_memory(layout, type, reset);
}

memory_ptr engine::share_buffer(const layout& layout, shared_handle buf) {
    shared_mem_params params = { shared_mem_type::shared_mem_buffer, nullptr, nullptr, buf,
#ifdef _WIN32
        nullptr,
#else
        0,
#endif
        0 };
    return reinterpret_handle(layout, params);
}

memory_ptr engine::share_usm(const layout& layout, shared_handle usm_ptr) {
    shared_mem_params params = { shared_mem_type::shared_mem_usm, nullptr, nullptr, usm_ptr,
#ifdef _WIN32
        nullptr,
#else
        0,
#endif
        0 };
    return reinterpret_handle(layout, params);
}

memory::ptr engine::share_image(const layout& layout, shared_handle img) {
    shared_mem_params params = { shared_mem_type::shared_mem_image, nullptr, nullptr, img,
#ifdef _WIN32
        nullptr,
#else
        0,
#endif
        0 };
    return reinterpret_handle(layout, params);
}

#ifdef _WIN32
memory_ptr engine::share_surface(const layout& layout, shared_handle surf, uint32_t plane) {
    shared_mem_params params = { shared_mem_type::shared_mem_vasurface, nullptr, nullptr, nullptr, surf, plane };
    return reinterpret_handle(layout, params);
}

memory_ptr engine::share_dx_buffer(const layout& layout, shared_handle res) {
    shared_mem_params params = { shared_mem_type::shared_mem_dxbuffer, nullptr, nullptr, res, nullptr, 0 };
    return reinterpret_handle(layout, params);
}
#else
memory_ptr engine::share_surface(const layout& layout, shared_surface surf, uint32_t plane) {
    shared_mem_params params = { shared_mem_type::shared_mem_vasurface, nullptr, nullptr, nullptr, surf, plane };
    return reinterpret_handle(layout, params);
}
#endif  // _WIN32

uint64_t engine::get_max_used_device_memory() const {
    std::lock_guard<std::mutex> guard(_mutex);
    uint64_t total_peak_memory_usage {0};
    for (auto const& m : _peak_memory_usage_map) {
        total_peak_memory_usage += m.second.load();
    }
    return total_peak_memory_usage;
}

uint64_t engine::get_max_used_device_memory(allocation_type type) const {
    std::lock_guard<std::mutex> guard(_mutex);
    uint64_t peak_memory_usage {0};
    auto iter = _peak_memory_usage_map.find(type);
    if (iter != _peak_memory_usage_map.end()) {
        peak_memory_usage = iter->second.load();
    }
    return peak_memory_usage;
}

uint64_t engine::get_used_device_memory(allocation_type type) const {
    std::lock_guard<std::mutex> guard(_mutex);
    uint64_t memory_usage {0};
    auto iter = _memory_usage_map.find(type);
    if (iter != _memory_usage_map.end()) {
        memory_usage = iter->second.load();
    }
    return memory_usage;
}

std::map<std::string, uint64_t> engine::get_memory_statistics() const {
    std::lock_guard<std::mutex> guard(_mutex);
    std::map<std::string, uint64_t> statistics;
    for (auto const& m : _memory_usage_map) {
        std::ostringstream oss;
        oss << m.first;
        statistics[oss.str()] = m.second.load();
    }
    return statistics;
}

void engine::add_memory_used(uint64_t bytes, allocation_type type) {
    std::lock_guard<std::mutex> guard(_mutex);
    if (!_memory_usage_map.count(type) && !_peak_memory_usage_map.count(type)) {
        _memory_usage_map[type] = 0;
        _peak_memory_usage_map[type] = 0;
    }
    _memory_usage_map[type] += bytes;
    if (_memory_usage_map[type] > _peak_memory_usage_map[type]) {
        _peak_memory_usage_map[type] = _memory_usage_map[type].load();
    }
}

void engine::subtract_memory_used(uint64_t bytes, allocation_type type) {
    std::lock_guard<std::mutex> guard(_mutex);
    auto iter = _memory_usage_map.find(type);
    if (iter != _memory_usage_map.end()) {
        _memory_usage_map[type] -= bytes;
    } else {
        throw std::runtime_error("Attempt to free unallocated memory");
    }
}

std::shared_ptr<cldnn::engine> engine::create(engine_types engine_type, runtime_types runtime_type, const device::ptr device) {
    std::shared_ptr<cldnn::engine> ret;
    switch (engine_type) {
    case engine_types::ocl:
        ret = ocl::create_ocl_engine(device, runtime_type);
        break;
    default:
        throw std::runtime_error("Invalid engine type");
    }
    const auto& info = device->get_info();
    GPU_DEBUG_INFO << "Selected Device: " << info.dev_name << std::endl;
    return ret;
}

std::shared_ptr<cldnn::engine> engine::create(engine_types engine_type, runtime_types runtime_type) {
    device_query query(engine_type, runtime_type);
    auto devices = query.get_available_devices();

    OPENVINO_ASSERT(!devices.empty(), "[GPU] Can't create ", engine_type, " engine for ", runtime_type, " runtime as no suitable devices are found\n"
                                      "[GPU] Please check OpenVINO documentation for GPU drivers setup guide.\n");

    auto iter = devices.find(std::to_string(device_query::device_id));
    auto& device = iter != devices.end() ? iter->second : devices.begin()->second;

    return engine::create(engine_type, runtime_type, device);
}

}  // namespace cldnn
