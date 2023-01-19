#include "systems_manager.h"

#include "core/logger.h"
#include "containers/darray.h"

// Systems
#include "core/kmemory.h"
#include "core/engine.h"
#include "core/console.h"
#include "core/kvar.h"
#include "core/event.h"
#include "core/input.h"
#include "platform/platform.h"
#include "systems/resource_system.h"
#include "systems/shader_system.h"
#include "renderer/renderer_frontend.h"
#include "systems/job_system.h"
#include "systems/texture_system.h"
#include "systems/camera_system.h"
#include "systems/render_view_system.h"
#include "systems/material_system.h"
#include "systems/geometry_system.h"

static b8 register_known_systems_pre_boot(systems_manager_state* state, application_config* app_config);
static b8 register_known_systems_post_boot(systems_manager_state* state, application_config* app_config);
static void shutdown_known_systems(systems_manager_state* state);

b8 systems_manager_initialize(systems_manager_state* state, application_config* app_config) {
    // Create a linear allocator for all systems (except memory) to use.
    linear_allocator_create(MEBIBYTES(64), 0, &state->systems_allocator);

    // Register known systems
    return register_known_systems_pre_boot(state, app_config);
}

b8 systems_manager_post_boot_initialize(systems_manager_state* state, application_config* app_config) {
    return register_known_systems_post_boot(state, app_config);
}

void systems_manager_shutdown(systems_manager_state* state) {
    shutdown_known_systems(state);
}

b8 systems_manager_update(systems_manager_state* state, u32 delta_time) {
    for (u32 i = 0; i < K_SYSTEM_TYPE_MAX_COUNT; ++i) {
        k_system* s = &state->systems[i];
        if (s->update) {
            if (!s->update(s->state, delta_time)) {
                KERROR("System update failed for type: %i", i);
            }
        }
    }
    return true;
}

b8 systems_manager_register(
    systems_manager_state* state,
    u16 type,
    PFN_system_initialize initialize,
    PFN_system_shutdown shutdown,
    PFN_system_update update,
    void* config) {
    k_system sys;
    sys.initialize = initialize;
    sys.shutdown = shutdown;
    sys.update = update;

    // Call initialize, alloc memory, call initialize again w/ allocated block.
    if (sys.initialize) {
        if (!sys.initialize(&sys.state_size, 0, config)) {
            KERROR("Failed to register system - initialize call failed.");
            return false;
        }

        sys.state = linear_allocator_allocate(&state->systems_allocator, sys.state_size);

        if (!sys.initialize(&sys.state_size, sys.state, config)) {
            KERROR("Failed to register system - initialize call failed.");
            return false;
        }
    } else {
        if (type != K_SYSTEM_TYPE_MEMORY) {
            KERROR("Initialize is required for types except K_SYSTEM_TYPE_MEMORY.");
            return false;
        }
    }

    state->systems[type] = sys;

    return true;
}

b8 register_known_systems_pre_boot(systems_manager_state* state, application_config* app_config) {
    // Memory
    if (!systems_manager_register(state, K_SYSTEM_TYPE_MEMORY, 0, memory_system_shutdown, 0, 0)) {
        KERROR("Failed to register memory system.");
        return false;
    }

    // Console
    if (!systems_manager_register(state, K_SYSTEM_TYPE_CONSOLE, console_initialize, console_shutdown, 0, 0)) {
        KERROR("Failed to register console system.");
        return false;
    }

    // KVars
    if (!systems_manager_register(state, K_SYSTEM_TYPE_KVAR, kvar_initialize, kvar_shutdown, 0, 0)) {
        KERROR("Failed to register KVar system.");
        return false;
    }

    // Events
    if (!systems_manager_register(state, K_SYSTEM_TYPE_EVENT, event_system_initialize, event_system_shutdown, 0, 0)) {
        KERROR("Failed to register event system.");
        return false;
    }

    // Logging
    if (!systems_manager_register(state, K_SYSTEM_TYPE_LOGGING, logging_initialize, logging_shutdown, 0, 0)) {
        KERROR("Failed to register logging system.");
        return false;
    }

    // Input
    if (!systems_manager_register(state, K_SYSTEM_TYPE_INPUT, input_system_initialize, input_system_shutdown, 0, 0)) {
        KERROR("Failed to register input system.");
        return false;
    }

    // Platform
    platform_system_config plat_config = {0};
    plat_config.application_name = app_config->name;
    plat_config.x = app_config->start_pos_x;
    plat_config.y = app_config->start_pos_y;
    plat_config.width = app_config->start_width;
    plat_config.height = app_config->start_height;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_PLATFORM, platform_system_startup, platform_system_shutdown, 0, &plat_config)) {
        KERROR("Failed to register platform system.");
        return false;
    }

    // Resource system.
    resource_system_config resource_sys_config;
    resource_sys_config.asset_base_path = "../assets";  // TODO: The application should probably configure this.
    resource_sys_config.max_loader_count = 32;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_RESOURCE, resource_system_initialize, resource_system_shutdown, 0, &resource_sys_config)) {
        KERROR("Failed to register resource system.");
        return false;
    }

    // Shader system
    shader_system_config shader_sys_config;
    shader_sys_config.max_shader_count = 1024;
    shader_sys_config.max_uniform_count = 128;
    shader_sys_config.max_global_textures = 31;
    shader_sys_config.max_instance_textures = 31;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_SHADER, shader_system_initialize, shader_system_shutdown, 0, &shader_sys_config)) {
        KERROR("Failed to register shader system.");
        return false;
    }

    // Renderer system
    renderer_system_config renderer_sys_config = {0};
    renderer_sys_config.application_name = app_config->name;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_RENDERER, renderer_system_initialize, renderer_system_shutdown, 0, &renderer_sys_config)) {
        KERROR("Failed to register renderer system.");
        return false;
    }

    b8 renderer_multithreaded = renderer_is_multithreaded();

    // This is really a core count. Subtract 1 to account for the main thread already being in use.
    i32 thread_count = platform_get_processor_count() - 1;
    if (thread_count < 1) {
        KFATAL("Error: Platform reported processor count (minus one for main thread) as %i. Need at least one additional thread for the job system.", thread_count);
        return false;
    } else {
        KTRACE("Available threads: %i", thread_count);
    }

    // Cap the thread count.
    const i32 max_thread_count = 15;
    if (thread_count > max_thread_count) {
        KTRACE("Available threads on the system is %i, but will be capped at %i.", thread_count, max_thread_count);
        thread_count = max_thread_count;
    }

    // Initialize the job system.
    // Requires knowledge of renderer multithread support, so should be initialized here.
    u32 job_thread_types[15];
    for (u32 i = 0; i < 15; ++i) {
        job_thread_types[i] = JOB_TYPE_GENERAL;
    }

    if (max_thread_count == 1 || !renderer_multithreaded) {
        // Everything on one job thread.
        job_thread_types[0] |= (JOB_TYPE_GPU_RESOURCE | JOB_TYPE_RESOURCE_LOAD);
    } else if (max_thread_count == 2) {
        // Split things between the 2 threads
        job_thread_types[0] |= JOB_TYPE_GPU_RESOURCE;
        job_thread_types[1] |= JOB_TYPE_RESOURCE_LOAD;
    } else {
        // Dedicate the first 2 threads to these things, pass off general tasks to other threads.
        job_thread_types[0] = JOB_TYPE_GPU_RESOURCE;
        job_thread_types[1] = JOB_TYPE_RESOURCE_LOAD;
    }

    job_system_config job_sys_config = {0};
    job_sys_config.max_job_thread_count = thread_count;
    job_sys_config.type_masks = job_thread_types;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_JOB, job_system_initialize, job_system_shutdown, job_system_update, &job_sys_config)) {
        KERROR("Failed to register job system.");
        return false;
    }

    return true;
}

void shutdown_known_systems(systems_manager_state* state) {
    state->systems[K_SYSTEM_TYPE_CAMERA].shutdown(state->systems[K_SYSTEM_TYPE_CAMERA].state);
    state->systems[K_SYSTEM_TYPE_FONT].shutdown(state->systems[K_SYSTEM_TYPE_FONT].state);

    state->systems[K_SYSTEM_TYPE_RENDER_VIEW].shutdown(state->systems[K_SYSTEM_TYPE_RENDER_VIEW].state);
    state->systems[K_SYSTEM_TYPE_GEOMETRY].shutdown(state->systems[K_SYSTEM_TYPE_GEOMETRY].state);
    state->systems[K_SYSTEM_TYPE_MATERIAL].shutdown(state->systems[K_SYSTEM_TYPE_MATERIAL].state);
    state->systems[K_SYSTEM_TYPE_TEXTURE].shutdown(state->systems[K_SYSTEM_TYPE_TEXTURE].state);

    state->systems[K_SYSTEM_TYPE_JOB].shutdown(state->systems[K_SYSTEM_TYPE_JOB].state);
    state->systems[K_SYSTEM_TYPE_SHADER].shutdown(state->systems[K_SYSTEM_TYPE_SHADER].state);
    state->systems[K_SYSTEM_TYPE_RENDERER].shutdown(state->systems[K_SYSTEM_TYPE_RENDERER].state);

    state->systems[K_SYSTEM_TYPE_RESOURCE].shutdown(state->systems[K_SYSTEM_TYPE_RESOURCE].state);
    state->systems[K_SYSTEM_TYPE_PLATFORM].shutdown(state->systems[K_SYSTEM_TYPE_PLATFORM].state);
    state->systems[K_SYSTEM_TYPE_INPUT].shutdown(state->systems[K_SYSTEM_TYPE_INPUT].state);
    state->systems[K_SYSTEM_TYPE_LOGGING].shutdown(state->systems[K_SYSTEM_TYPE_LOGGING].state);
    state->systems[K_SYSTEM_TYPE_EVENT].shutdown(state->systems[K_SYSTEM_TYPE_EVENT].state);
    state->systems[K_SYSTEM_TYPE_KVAR].shutdown(state->systems[K_SYSTEM_TYPE_KVAR].state);
    state->systems[K_SYSTEM_TYPE_CONSOLE].shutdown(state->systems[K_SYSTEM_TYPE_CONSOLE].state);

    state->systems[K_SYSTEM_TYPE_MEMORY].shutdown(state->systems[K_SYSTEM_TYPE_MEMORY].state);
}

b8 register_known_systems_post_boot(systems_manager_state* state, application_config* app_config) {
    // Texture system.
    texture_system_config texture_sys_config;
    texture_sys_config.max_texture_count = 65536;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_TEXTURE, texture_system_initialize, texture_system_shutdown, 0, &texture_sys_config)) {
        KERROR("Failed to register texture system.");
        return false;
    }

    // Font system.
    if (!systems_manager_register(state, K_SYSTEM_TYPE_FONT, font_system_initialize, font_system_shutdown, 0, &app_config->font_config)) {
        KERROR("Failed to register font system.");
        return false;
    }

    // Camera
    camera_system_config camera_sys_config;
    camera_sys_config.max_camera_count = 61;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_CAMERA, camera_system_initialize, camera_system_shutdown, 0, &camera_sys_config)) {
        KERROR("Failed to register camera system.");
        return false;
    }

    render_view_system_config render_view_sys_config = {};
    render_view_sys_config.max_view_count = 251;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_RENDER_VIEW, render_view_system_initialize, render_view_system_shutdown, 0, &render_view_sys_config)) {
        KERROR("Failed to register render view system.");
        return false;
    }

    // Load render views from app config.
    u32 view_count = darray_length(app_config->render_views);
    for (u32 v = 0; v < view_count; ++v) {
        render_view_config* view = &app_config->render_views[v];
        if (!render_view_system_create(view)) {
            KFATAL("Failed to create view '%s'. Aborting application.", view->name);
            return false;
        }
    }

    // Material system.
    material_system_config material_sys_config;
    material_sys_config.max_material_count = 4096;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_MATERIAL, material_system_initialize, material_system_shutdown, 0, &material_sys_config)) {
        KERROR("Failed to register material system.");
        return false;
    }

    // Geometry system.
    geometry_system_config geometry_sys_config;
    geometry_sys_config.max_geometry_count = 4096;
    if (!systems_manager_register(state, K_SYSTEM_TYPE_GEOMETRY, geometry_system_initialize, geometry_system_shutdown, 0, &geometry_sys_config)) {
        KERROR("Failed to register geometry system.");
        return false;
    }

    return true;
}