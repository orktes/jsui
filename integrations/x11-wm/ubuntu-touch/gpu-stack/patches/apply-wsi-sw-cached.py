# Turnip on KGSL has HOST_COHERENT (uncached) or HOST_CACHED (non-coherent) memory, never
# both. The sw Wayland WSI asks for cached+coherent, falls back to uncached, and then memcpys
# each frame out of uncached memory (~100 ms for 2000x1130). Prefer cached memory and
# invalidate it before the copy.
import sys
root = sys.argv[1] + "/src/vulkan/wsi/"
def sub(f, old, new):
    p = root + f; s = open(p).read()
    if new in s: return
    assert old in s, (f, old); open(p, "w").write(s.replace(old, new, 1))
sub("wsi_common.h", "   WSI_CB(AllocateMemory);\n", "   WSI_CB(AllocateMemory);\n   WSI_CB(InvalidateMappedMemoryRanges);\n")
sub("wsi_common.c", "   WSI_GET_CB(AllocateMemory);\n", "   WSI_GET_CB(AllocateMemory);\n   WSI_GET_CB(InvalidateMappedMemoryRanges);\n")
sub("wsi_common.c",
    "   VkMemoryPropertyFlags req_props = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;\n\n   if (wsi->sw)\n",
    "   VkMemoryPropertyFlags req_props = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;\n\n"
    "   if (wsi->sw) {\n"
    "      /* Cached but not coherent beats uncached for CPU readback; the\n"
    "       * Wayland memcpy path invalidates before reading. */\n"
    "      u_foreach_bit(t, type_bits) {\n"
    "         VkMemoryPropertyFlags f = wsi->memory_props.memoryTypes[t].propertyFlags;\n"
    "         if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))\n"
    "            return t;\n"
    "      }\n"
    "   }\n"
    "   if (wsi->sw)\n")
sub("wsi_common_wayland.c",
    "      struct wsi_wl_image *image = &chain->images[image_index];\n      memcpy(image->shm_ptr,",
    "      struct wsi_wl_image *image = &chain->images[image_index];\n"
    "      const VkMappedMemoryRange range = {\n"
    "         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,\n"
    "         .memory = image->base.blit.memory ? image->base.blit.memory : image->base.memory,\n"
    "         .size = VK_WHOLE_SIZE,\n"
    "      };\n"
    "      chain->base.wsi->InvalidateMappedMemoryRanges(chain->base.device, 1, &range);\n"
    "      memcpy(image->shm_ptr,")
print("patched wsi")
