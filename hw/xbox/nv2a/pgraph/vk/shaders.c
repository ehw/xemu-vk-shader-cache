/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qemu/mstring.h"
#include "renderer.h"
#include "xemu-version.h"

#define VSH_UBO_BINDING 0
#define PSH_UBO_BINDING 1
#define PSH_TEX_BINDING 2

const size_t MAX_UNIFORM_ATTR_VALUES_SIZE = NV2A_VERTEXSHADER_ATTRIBUTES * 4 * sizeof(float);

static const char *shader_vk_driver = NULL;

static const char *shader_vk_get_base_path(void)
{
    extern const char *xemu_settings_get_base_path(void);
    return xemu_settings_get_base_path();
}

bool shader_vk_cache_enabled(void)
{
    #ifdef HAVE_UI_SETTINGS
    extern struct config g_config;
    return g_config.perf.cache_shaders;
    #else
    return true;
    #endif
}

static void shader_vk_create_cache_folder(void)
{
    char *shader_path = g_strdup_printf("%sshaders/vk", shader_vk_get_base_path());
    qemu_mkdir(shader_path);
    g_free(shader_path);
}

static char *shader_vk_get_cache_directory(uint64_t hash)
{
    const char *cfg_dir = shader_vk_get_base_path();
    char *shader_cache_dir = g_build_filename(cfg_dir, "shaders", "vk",
        g_strdup_printf("%04x", (uint32_t)(hash >> 48)), NULL);
    return shader_cache_dir;
}

static char *shader_vk_get_lru_cache_path(void)
{
    return g_build_filename(shader_vk_get_base_path(), "shaders", "vk", "shader_cache_list", NULL);
}
static char *shader_vk_get_binary_path(const char *shader_cache_dir, uint64_t hash)
{
    uint64_t bin_mask = (uint64_t)0xffff << 48;
    char *filename = g_strdup_printf("%012" PRIx64, hash & ~bin_mask);
    char *result = g_build_filename(shader_cache_dir, filename, NULL);
    g_free(filename);
    return result;
}

static bool shader_vk_load_from_disk(PGRAPHState *pg, uint64_t hash, ShaderModuleInfo **out_module)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    
    char *shader_cache_dir = shader_vk_get_cache_directory(hash);
    char *shader_path = shader_vk_get_binary_path(shader_cache_dir, hash);
    char *cached_xemu_version = NULL;
    char *cached_vk_driver = NULL;
    GByteArray *spirv_data = NULL;
    uint8_t *temp_spirv_data = NULL;

    uint64_t cached_xemu_version_len;
    uint64_t vk_driver_len;
    uint32_t spv_size;

    g_free(shader_cache_dir);

    FILE *shader_file = qemu_fopen(shader_path, "rb");
    if (!shader_file) {
        goto error;
    }

    #define VK_READ_OR_ERR(data, data_len) \
        do { \
            size_t nread = fread(data, data_len, 1, shader_file); \
            if (nread != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    VK_READ_OR_ERR(&cached_xemu_version_len, sizeof(cached_xemu_version_len));

    cached_xemu_version = g_malloc0(cached_xemu_version_len + 1);
    VK_READ_OR_ERR(cached_xemu_version, cached_xemu_version_len);
    if (strcmp(cached_xemu_version, xemu_version) != 0) {
        fclose(shader_file);
        goto error;
    }

    VK_READ_OR_ERR(&vk_driver_len, sizeof(vk_driver_len));

    cached_vk_driver = g_malloc0(vk_driver_len);
    VK_READ_OR_ERR(cached_vk_driver, vk_driver_len);
    if (strcmp(cached_vk_driver, shader_vk_driver) != 0) {
        fclose(shader_file);
        goto error;
    }

    // Read only the hash of the key, not the entire key structure
    uint64_t key_hash;
    VK_READ_OR_ERR(&key_hash, sizeof(key_hash));
    VK_READ_OR_ERR(&spv_size, sizeof(spv_size));

    temp_spirv_data = g_malloc(spv_size);
    VK_READ_OR_ERR(temp_spirv_data, (size_t)spv_size);
    
    spirv_data = g_byte_array_new_take(temp_spirv_data, spv_size);

    #undef VK_READ_OR_ERR

    fclose(shader_file);
    g_free(shader_path);
    g_free(cached_xemu_version);
    g_free(cached_vk_driver);

    // Create a proper ShaderModuleInfo structure for the loaded shader
    ShaderModuleInfo *info = g_malloc0(sizeof(*info));
    info->refcnt = 0;
    info->spirv = spirv_data;
    
    // Create the Vulkan shader module
    info->module = pgraph_vk_create_shader_module_from_spv(r, info->spirv);
    if (info->module == VK_NULL_HANDLE) {
        g_free(info);
        return false;
    }
    
    // Initialize reflection data for the loaded shader
    pgraph_vk_init_layout_from_spv(info);
    
    *out_module = info;
    return true;

error:
    /* Delete the shader so it won't be loaded again */
    qemu_unlink(shader_path);
    g_free(shader_path);
    if (spirv_data) {
        g_byte_array_unref(spirv_data);
    } else if (temp_spirv_data) {
        // temp_spirv_data was allocated but not yet converted to GByteArray
        g_free(temp_spirv_data);
    }
    g_free(cached_xemu_version);
    g_free(cached_vk_driver);
    return false;
}

static void *shader_vk_write_to_disk(void *arg)
{
    ShaderModuleCacheEntry *module = (ShaderModuleCacheEntry*) arg;
    
    if (!shader_vk_cache_enabled()) {
        return NULL;
    }

    char *shader_cache_dir = shader_vk_get_cache_directory(module->node.hash);
    char *shader_path = shader_vk_get_binary_path(shader_cache_dir, module->node.hash);
    
    uint64_t vk_driver_len = (uint64_t) (strlen(shader_vk_driver) + 1);
    uint64_t xemu_version_len = (uint64_t) (strlen(xemu_version) + 1);

    qemu_mkdir(shader_cache_dir);
    g_free(shader_cache_dir);

    FILE *shader_file = qemu_fopen(shader_path, "wb");
    if (!shader_file) {
        goto error;
    }

    #define VK_WRITE_OR_ERR(data, data_size) \
        do { \
            size_t written = fwrite(data, data_size, 1, shader_file); \
            if (written != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    VK_WRITE_OR_ERR(&xemu_version_len, sizeof(xemu_version_len));
    VK_WRITE_OR_ERR(xemu_version, xemu_version_len);

    VK_WRITE_OR_ERR(&vk_driver_len, sizeof(vk_driver_len));
    VK_WRITE_OR_ERR(shader_vk_driver, vk_driver_len);

    // Write the cache entry hash (already computed and stored in node)
    VK_WRITE_OR_ERR(&module->node.hash, sizeof(module->node.hash));
    
    uint32_t spv_size = module->module_info->spirv->len;
    VK_WRITE_OR_ERR(&spv_size, sizeof(spv_size));
    VK_WRITE_OR_ERR(module->module_info->spirv->data, spv_size);

    #undef VK_WRITE_OR_ERR

    fclose(shader_file);
    g_free(shader_path);

    return NULL;

error:
    g_free(shader_path);
    return NULL;
}

static void shader_vk_cache_to_disk(ShaderModuleCacheEntry *module)
{
    if (!shader_vk_cache_enabled()) {
        return;
    }

 
    // Call the write function directly instead of in a background thread
    // This ensures shaders are saved before program exits
    shader_vk_write_to_disk(module);
}

static void lru_write_hash_callback(Lru *lru, LruNode *node, void *opaque)
{
    FILE *lru_list_file = (FILE*) opaque;
    fwrite(&node->hash, sizeof(uint64_t), 1, lru_list_file);
}

struct write_data {
    FILE *file;
    GHashTable *existing_hashes;
    GHashTable *added_hashes;
};

static void lru_check_and_write_hash_callback(Lru *lru, LruNode *node, void *opaque)
{
    struct write_data *data = (struct write_data*) opaque;
    uint64_t hash = node->hash;
    
    if (!g_hash_table_contains(data->existing_hashes, (gpointer)hash) &&
        !g_hash_table_contains(data->added_hashes, (gpointer)hash)) {
        fwrite(&hash, sizeof(uint64_t), 1, data->file);
        g_hash_table_add(data->added_hashes, (gpointer)hash);
    }
}

static void shader_vk_reload_lru_from_disk(PGRAPHState *pg)
{
    if (!shader_vk_cache_enabled()) {
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    char *shader_lru_path = shader_vk_get_lru_cache_path();

    FILE *lru_shaders_list = qemu_fopen(shader_lru_path, "rb");
    g_free(shader_lru_path);
    if (!lru_shaders_list) {
        return;
    }

    uint64_t hash;
    while (fread(&hash, sizeof(uint64_t), 1, lru_shaders_list) == 1) {
        ShaderModuleInfo *cached_module = NULL;
        if (shader_vk_load_from_disk(pg, hash, &cached_module)) {
            NV2A_VK_DPRINTF("Loaded cached shader from disk (hash: %llx)", 
                          (unsigned long long)hash);
        } else {
        }
    }

    fclose(lru_shaders_list);
}

void pgraph_vk_shader_cache_write_reload_list(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!shader_vk_cache_enabled()) {
        qatomic_set(&r->shader_cache_writeback_pending, false);
        qemu_event_set(&r->shader_cache_writeback_complete);
        return;
    }

    char *shader_lru_path = shader_vk_get_lru_cache_path();

    GHashTable *existing_hashes = g_hash_table_new(g_direct_hash, g_direct_equal);
    FILE *existing_list = qemu_fopen(shader_lru_path, "rb");
    if (existing_list) {
        uint64_t hash;
        while (fread(&hash, sizeof(uint64_t), 1, existing_list) == 1) {
            g_hash_table_add(existing_hashes, (gpointer)hash);
        }
        fclose(existing_list);
    }

    FILE *lru_list = qemu_fopen(shader_lru_path, "ab");
    g_free(shader_lru_path);
    if (!lru_list) {
        g_hash_table_destroy(existing_hashes);
        return;
    }

    // Only write hashes that aren't already in the persistent list
    GHashTable *added_hashes = g_hash_table_new(g_direct_hash, g_direct_equal);
    lru_visit_active(&r->shader_module_cache, lru_check_and_write_hash_callback, 
                     &(struct write_data){.file = lru_list, .existing_hashes = existing_hashes, .added_hashes = added_hashes});
    
    fclose(lru_list);
    g_hash_table_destroy(existing_hashes);
    g_hash_table_destroy(added_hashes);
    
    qatomic_set(&r->shader_cache_writeback_pending, false);
    qemu_event_set(&r->shader_cache_writeback_complete);
}

static void create_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    size_t num_sets = ARRAY_SIZE(r->descriptor_sets);

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 2 * num_sets,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = NV2A_MAX_TEXTURES * num_sets,
        }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = ARRAY_SIZE(pool_sizes),
        .pPoolSizes = pool_sizes,
        .maxSets = ARRAY_SIZE(r->descriptor_sets),
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };
    VK_CHECK(vkCreateDescriptorPool(r->device, &pool_info, NULL,
                                    &r->descriptor_pool));
}

static void destroy_descriptor_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
    r->descriptor_pool = VK_NULL_HANDLE;
}

static void create_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayoutBinding bindings[2 + NV2A_MAX_TEXTURES];

    bindings[0] = (VkDescriptorSetLayoutBinding){
        .binding = VSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    bindings[1] = (VkDescriptorSetLayoutBinding){
        .binding = PSH_UBO_BINDING,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        bindings[2 + i] = (VkDescriptorSetLayoutBinding){
            .binding = PSH_TEX_BINDING + i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = ARRAY_SIZE(bindings),
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &layout_info, NULL,
                                         &r->descriptor_set_layout));
}

static void destroy_descriptor_set_layout(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyDescriptorSetLayout(r->device, r->descriptor_set_layout, NULL);
    r->descriptor_set_layout = VK_NULL_HANDLE;
}

static void create_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDescriptorSetLayout layouts[ARRAY_SIZE(r->descriptor_sets)];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        layouts[i] = r->descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = r->descriptor_pool,
        .descriptorSetCount = ARRAY_SIZE(r->descriptor_sets),
        .pSetLayouts = layouts,
    };
    VK_CHECK(
        vkAllocateDescriptorSets(r->device, &alloc_info, r->descriptor_sets));
}

static void destroy_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeDescriptorSets(r->device, r->descriptor_pool,
                         ARRAY_SIZE(r->descriptor_sets), r->descriptor_sets);
    for (int i = 0; i < ARRAY_SIZE(r->descriptor_sets); i++) {
        r->descriptor_sets[i] = VK_NULL_HANDLE;
    }
}

void pgraph_vk_update_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    bool need_uniform_write =
        r->uniforms_changed ||
        !r->storage_buffers[BUFFER_UNIFORM_STAGING].buffer_offset;

    if (!(r->shader_bindings_changed || r->texture_bindings_changed ||
          (r->descriptor_set_index == 0) || need_uniform_write)) {
        return; // Nothing changed
    }

    ShaderBinding *binding = r->shader_binding;
    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };
    VkDeviceSize ubo_buffer_total_size = 0;
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_total_size += layouts[i]->total_size;
    }
    bool need_ubo_staging_buffer_reset =
        r->uniforms_changed &&
        !pgraph_vk_buffer_has_space_for(pg, BUFFER_UNIFORM_STAGING,
                                        ubo_buffer_total_size,
                                        r->device_props.limits.minUniformBufferOffsetAlignment);

    bool need_descriptor_write_reset =
        (r->descriptor_set_index >= ARRAY_SIZE(r->descriptor_sets));

    if (need_descriptor_write_reset || need_ubo_staging_buffer_reset) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        need_uniform_write = true;
    }

    VkWriteDescriptorSet descriptor_writes[2 + NV2A_MAX_TEXTURES];

    assert(r->descriptor_set_index < ARRAY_SIZE(r->descriptor_sets));

    if (need_uniform_write) {
        for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
            void *data = layouts[i]->allocation;
            VkDeviceSize size = layouts[i]->total_size;
            r->uniform_buffer_offsets[i] = pgraph_vk_append_to_buffer(
                pg, BUFFER_UNIFORM_STAGING, &data, &size, 1,
                r->device_props.limits.minUniformBufferOffsetAlignment);
        }

        r->uniforms_changed = false;
    }

    VkDescriptorBufferInfo ubo_buffer_infos[2];
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        ubo_buffer_infos[i] = (VkDescriptorBufferInfo){
            .buffer = r->storage_buffers[BUFFER_UNIFORM].buffer,
            .offset = r->uniform_buffer_offsets[i],
            .range = layouts[i]->total_size,
        };
        descriptor_writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->descriptor_sets[r->descriptor_set_index],
            .dstBinding = i == 0 ? VSH_UBO_BINDING : PSH_UBO_BINDING,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &ubo_buffer_infos[i],
        };
    }

    VkDescriptorImageInfo image_infos[NV2A_MAX_TEXTURES];
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        image_infos[i] = (VkDescriptorImageInfo){
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = r->texture_bindings[i]->image_view,
            .sampler = r->texture_bindings[i]->sampler,
        };
        descriptor_writes[2 + i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->descriptor_sets[r->descriptor_set_index],
            .dstBinding = PSH_TEX_BINDING + i,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[i],
        };
    }

    vkUpdateDescriptorSets(r->device, 6, descriptor_writes, 0, NULL);

    r->descriptor_set_index++;
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    for (int i = 0; i < ARRAY_SIZE(binding->vsh.uniform_locs); i++) {
        binding->vsh.uniform_locs[i] = uniform_index(
            &binding->vsh.module_info->uniforms, VshUniformInfo[i].name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->psh.uniform_locs); i++) {
        binding->psh.uniform_locs[i] = uniform_index(
            &binding->psh.module_info->uniforms, PshUniformInfo[i].name);
    }
}

static ShaderModuleInfo *
get_and_ref_shader_module_for_key(PGRAPHVkState *r,
                                  const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_ref_shader_module(module->module_info);
    return module->module_info;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));

    NV2A_VK_DPRINTF("cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);

    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&binding->state.geom);
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = VK_SHADER_STAGE_GEOMETRY_BIT;
        key.geom.state = binding->state.geom;
        key.geom.glsl_opts.vulkan = true;
        binding->geom.module_info = get_and_ref_shader_module_for_key(r, &key);
    } else {
        binding->geom.module_info = NULL;
    }

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_VERTEX_BIT;
    key.vsh.state = binding->state.vsh;
    key.vsh.glsl_opts.vulkan = true;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    key.vsh.glsl_opts.use_push_constants_for_uniform_attrs =
        r->use_push_constants_for_uniform_attrs;
    key.vsh.glsl_opts.ubo_binding = VSH_UBO_BINDING;
    binding->vsh.module_info = get_and_ref_shader_module_for_key(r, &key);

    memset(&key, 0, sizeof(key));
    key.kind = VK_SHADER_STAGE_FRAGMENT_BIT;
    key.psh.state = binding->state.psh;
    key.psh.glsl_opts.vulkan = true;
    key.psh.glsl_opts.ubo_binding = PSH_UBO_BINDING;
    key.psh.glsl_opts.tex_binding = PSH_TEX_BINDING;
    binding->psh.module_info = get_and_ref_shader_module_for_key(r, &key);

    update_shader_uniform_locs(binding);
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_cache);
    ShaderBinding *snode = container_of(node, ShaderBinding, node);

    ShaderModuleInfo *modules[] = {
        snode->vsh.module_info,
        snode->geom.module_info,
        snode->psh.module_info,
    };
    for (int i = 0; i < ARRAY_SIZE(modules); i++) {
        if (modules[i]) {
            pgraph_vk_unref_shader_module(r, modules[i]);
        }
    }
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *snode = container_of(node, ShaderBinding, node);
    return memcmp(&snode->state, key, sizeof(ShaderState));
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{

    
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));
    


    if (shader_vk_cache_enabled() && r->current_pg_state) {
        ShaderModuleInfo *cached_module = NULL;
        if (shader_vk_load_from_disk(r->current_pg_state, node->hash, &cached_module)) {
            NV2A_VK_DPRINTF("Vulkan shader cache hit for hash %llx", (unsigned long long)node->hash);
            module->module_info = cached_module;
            pgraph_vk_ref_shader_module(module->module_info);
            return;
        }
    }

    NV2A_VK_DPRINTF("Vulkan shader cache miss - compiling shader");

    MString *code;

    switch (module->key.kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        code = NULL;
    }

    module->module_info = pgraph_vk_create_shader_module_from_glsl(
        r, module->key.kind, mstring_get_str(code));
    if (module->module_info) {
    } else {
        // Failed to compile shader
    }
    pgraph_vk_ref_shader_module(module->module_info);
    mstring_unref(code);

    // Save to persistent cache after successful compilation if caching is enabled
    if (shader_vk_cache_enabled() && r->current_pg_state) {
        shader_vk_cache_to_disk(module);
    }
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, shader_module_cache);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    pgraph_vk_unref_shader_module(r, module->module_info);
    module->module_info = NULL;
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static void shader_cache_init(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Initialize Vulkan shader cache locking
    qemu_mutex_init(&r->shader_cache_lock);
    qemu_event_init(&r->shader_cache_writeback_complete, false);
    r->shader_cache_writeback_pending = false;

    // Set up Vulkan driver info for cache compatibility checking
    if (!shader_vk_driver) {
        shader_vk_driver = (const char *) r->device_props.deviceName;
    }

    // Create shader cache directory
    if (shader_vk_cache_enabled()) {
        shader_vk_create_cache_folder();
    }

    const size_t shader_cache_size = 1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = g_malloc_n(shader_cache_size, sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }
    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50 * 1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache,
                     &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict =
        shader_module_cache_entry_post_evict;
}

static void shader_cache_finalize(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Save Vulkan shader cache before cleanup
    if (shader_vk_cache_enabled()) {
        pgraph_vk_shader_cache_write_reload_list(pg);
    }

    lru_flush(&r->shader_cache);
    g_free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;

    // Clean up Vulkan cache synchronization primitives
    qemu_mutex_destroy(&r->shader_cache_lock);
    qemu_event_destroy(&r->shader_cache_writeback_complete);
}

static ShaderBinding *get_shader_binding_for_state(PGRAPHVkState *r,
                                                   const ShaderState *state)
{
    uint64_t hash = fast_hash((void *)state, sizeof(*state));
    LruNode *node = lru_lookup(&r->shader_cache, hash, state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    NV2A_VK_DPRINTF("shader state hash: %016" PRIx64 " %p", hash, binding);
    return binding;
}

static void apply_uniform_updates(ShaderUniformLayout *layout,
                                  const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] != -1) {
            uniform_copy(layout, locs[i], (char*)values + info[i].val_offs,
                         4, (info[i].size * info[i].count) / 4);
        }
    }
}

// FIXME: Dirty tracking
static void update_shader_uniforms(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

    assert(r->shader_binding);
    ShaderBinding *binding = r->shader_binding;
    ShaderUniformLayout *layouts[] = { &binding->vsh.module_info->uniforms,
                                       &binding->psh.module_info->uniforms };

    VshUniformValues vsh_values;
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                  binding->vsh.uniform_locs, &vsh_values);
    apply_uniform_updates(&binding->vsh.module_info->uniforms, VshUniformInfo,
                          binding->vsh.uniform_locs, &vsh_values,
                          VshUniform__COUNT);

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->psh.uniform_locs,
                                       &psh_values);
    for (int i = 0; i < 4; i++) {
        assert(r->texture_bindings[i] != NULL);
        float scale = r->texture_bindings[i]->key.scale;

        BasicColorFormatInfo f_basic =
            kelvin_color_format_info_map[pg->vk_renderer_state
                                             ->texture_bindings[i]
                                             ->key.state.color_format];
        if (!f_basic.linear) {
            scale = 1.0;
        }

        psh_values.texScale[i] = scale;
    }
    apply_uniform_updates(&binding->psh.module_info->uniforms, PshUniformInfo,
                          binding->psh.uniform_locs, &psh_values,
                          PshUniform__COUNT);

    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        uint64_t hash =
            fast_hash(layouts[i]->allocation, layouts[i]->total_size);
        r->uniforms_changed |= (hash != r->uniform_buffer_hashes[i]);
        r->uniform_buffer_hashes[i] = hash;
    }

    nv2a_profile_inc_counter(r->uniforms_changed ?
                                 NV2A_PROF_SHADER_UBO_DIRTY :
                                 NV2A_PROF_SHADER_UBO_NOTDIRTY);

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_bind_shaders(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHVkState *r = pg->vk_renderer_state;

    r->shader_bindings_changed = false;

    if (!r->shader_binding ||
        pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        ShaderState new_state = pgraph_glsl_get_shader_state(pg);
        if (!r->shader_binding || memcmp(&r->shader_binding->state, &new_state,
                                         sizeof(ShaderState))) {
            r->shader_binding = get_shader_binding_for_state(r, &new_state);
            r->shader_bindings_changed = true;
        }
    } else {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
    }

    update_shader_uniforms(pg);

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_init_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Set reference to parent PGRAPHState for cache operations
    r->current_pg_state = pg;

    pgraph_vk_init_glsl_compiler();
    create_descriptor_pool(pg);
    create_descriptor_set_layout(pg);
    create_descriptor_sets(pg);
    shader_cache_init(pg);
	
    shader_vk_reload_lru_from_disk(pg);

    r->use_push_constants_for_uniform_attrs =
        (r->device_props.limits.maxPushConstantsSize >=
         MAX_UNIFORM_ATTR_VALUES_SIZE);
}

void pgraph_vk_finalize_shaders(PGRAPHState *pg)
{
    shader_cache_finalize(pg);
    destroy_descriptor_sets(pg);
    destroy_descriptor_set_layout(pg);
    destroy_descriptor_pool(pg);
    pgraph_vk_finalize_glsl_compiler();
}
