import sys
p = sys.argv[1]
s = open(p).read()

def rep(old, new, label):
    global s
    if old not in s:
        raise SystemExit("ANCHOR MISSING: " + label)
    s = s.replace(old, new, 1)

# 1) globals
rep("static VkInstance g_inst; static VkPhysicalDevice g_pdev; static VkDevice g_dev; static VkQueue g_queue;",
    "static VkInstance g_inst; static VkPhysicalDevice g_pdev; static VkDevice g_dev; static VkQueue g_queue;\n"
    "// A-62: CoreAnimation creates pipelines continuously while a window is dragged, and every creation pays for the\n"
    "// SPIR-V translation and the ANV compile again -- the profile is dominated by naga passes (dce_cf_list,\n"
    "// copy_prop_vars_cf_node, gather_vars_written, rspirv parse), not by drawing.  Metal hands the plugin a null\n"
    "// cache, so give it one of our own: identical states then come back out of the cache instead of being rebuilt.\n"
    "static VkPipelineCache g_pipeCache;\n"
    "static PFN_vkCreatePipelineCache pvkCreatePipelineCache;",
    "globals")

# 2) create it right after the device exists
rep('    VKCK(pvkCreateDevice(g_pdev, &dci, NULL, &g_dev), "vkCreateDevice");\n    DEVICE_FNS(LOAD_D)',
    '    VKCK(pvkCreateDevice(g_pdev, &dci, NULL, &g_dev), "vkCreateDevice");\n'
    '    DEVICE_FNS(LOAD_D)\n'
    '    pvkCreatePipelineCache = (PFN_vkCreatePipelineCache)pvkGetDeviceProcAddr(g_dev, "vkCreatePipelineCache");\n'
    '    if (pvkCreatePipelineCache && !g_pipeCache) {\n'
    '        VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, 0, NULL };\n'
    '        if (pvkCreatePipelineCache(g_dev, &pci, NULL, &g_pipeCache) == VK_SUCCESS)\n'
    '            nvlog("vk: internal pipeline cache created (Metal passes no cache)");\n'
    '        else { g_pipeCache = VK_NULL_HANDLE; nvlog("vk: pipeline cache unavailable"); }\n'
    '    }',
    "create cache")

# 3) graphics pipelines use it when the caller has none
rep("    VkResult result = pvkCreateGraphicsPipelines(dev, cache, count, info, allocator, &handle);",
    "    if (!cache) cache = g_pipeCache;\n"
    "    VkResult result = pvkCreateGraphicsPipelines(dev, cache, count, info, allocator, &handle);",
    "graphics")

open(p, "w").write(s)
print("patched")
