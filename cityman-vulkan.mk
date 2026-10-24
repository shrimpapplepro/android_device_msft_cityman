# Cityman Adreno 430 Vulkan ICD extra install paths.
#
# cityman-vendor.mk already copies:
#   vendor/msft/cityman/proprietary/vendor/lib/hw/vulkan.msm8994.so
#     -> $(TARGET_COPY_OUT_VENDOR)/lib/hw/vulkan.msm8994.so
#   vendor/msft/cityman/proprietary/vendor/lib64/hw/vulkan.msm8994.so
#     -> $(TARGET_COPY_OUT_VENDOR)/lib64/hw/vulkan.msm8994.so
# SPHAL / libvulkan searches /vendor/lib{,64}/vulkan.msm8994.so (without hw/).
#
# Feature XML stays android.hardware.vulkan.version-1_0_3 (device.mk).
# Do not advertise 1.1 or 1.4. Do not set ro.hwui.use_vulkan.

PRODUCT_PACKAGES += \
    cityman-vk-probe \
    cityman-vk-probe32 \
    cityman-vk-tri \
    cityman-vk-clear \
    cityman-vk-caps \
    cityman-vk-fence \
    cityman-vk-mem \
    cityman-vk-cap \

PRODUCT_COPY_FILES += \
    vendor/msft/cityman/proprietary/vendor/lib/hw/vulkan.msm8994.so:$(TARGET_COPY_OUT_VENDOR)/lib/vulkan.msm8994.so \
    vendor/msft/cityman/proprietary/vendor/lib64/hw/vulkan.msm8994.so:$(TARGET_COPY_OUT_VENDOR)/lib64/vulkan.msm8994.so
