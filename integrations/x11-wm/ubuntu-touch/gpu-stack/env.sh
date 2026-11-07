# source me: selects the ~/gpu-stack Mesa (Turnip on KGSL + Zink) instead of hybris / system Mesa
G=$HOME/gpu-stack
export LD_LIBRARY_PATH=$G/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export VK_ICD_FILENAMES=$G/share/vulkan/icd.d/freedreno_icd.aarch64.json
export VK_DRIVER_FILES=$VK_ICD_FILENAMES
export __EGL_VENDOR_LIBRARY_FILENAMES=$G/share/glvnd/egl_vendor.d/50_mesa.json
export MESA_LOADER_DRIVER_OVERRIDE=zink
export MESA_ZINK_NODEVICE=1   # local Mesa patch: zink without a DRM render node
export TU_DEBUG=noconform     # A610 has no HW multiview; without this Turnip says Vulkan 1.0
unset HYBRIS_LD_LIBRARY_PATH HYBRIS_VULKANPLATFORM EGL_PLATFORM LIBGL_ALWAYS_SOFTWARE
