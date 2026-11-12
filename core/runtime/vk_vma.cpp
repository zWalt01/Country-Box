// From fe3h-pc-recomp runtime/vk_vma.cpp @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied unchanged.
// From totk-pc-recomp runtime/vk_vma.cpp @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied unchanged. Country-Box: the vendored header's own warnings under clang are silenced around it (Linux).
// Vulkan Memory Allocator implementation (AMD, MIT; from the MSYS2 package vulkan-memory-allocator, not copied).
// VMA is C++ only, so this is the one C++ file of the runtime; the rest uses the C interface of vk_mem_alloc.h.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vulkan/vulkan.h>
// VMA 3.4.0 as vendored (third_party/vma): its nullability annotations are incomplete and two fields are unused under
// clang; they are VMA's, not ours, and stay as they are.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop
