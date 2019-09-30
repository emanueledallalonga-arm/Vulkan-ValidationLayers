/* Copyright (c) 2019 The Khronos Group Inc.
 * Copyright (c) 2019 Valve Corporation
 * Copyright (c) 2019 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: John Zulauf <jzulauf@lunarg.com>
 */

#include <vector>
#include "synchronization_validation.h"

static const char *string_SyncHazardVUID(SyncHazard hazard) {
    switch (hazard) {
        case SyncHazard::NONE:
            return "SYNC-NONE";
            break;
        case SyncHazard::READ_AFTER_WRITE:
            return "SYNC-HAZARD-READ_AFTER_WRITE";
            break;
        case SyncHazard::WRITE_AFTER_READ:
            return "SYNC-HAZARD-WRITE_AFTER_READ";
            break;
        case SyncHazard::WRITE_AFTER_WRITE:
            return "SYNC-HAZARD-WRITE_AFTER_WRITE";
            break;
        default:
            assert(0);
    }
    return "SYNC-HAZARD-INVALID";
}

static const char *string_SyncHazard(SyncHazard hazard) {
    switch (hazard) {
        case SyncHazard::NONE:
            return "NONR";
            break;
        case SyncHazard::READ_AFTER_WRITE:
            return "READ_AFTER_WRITE";
            break;
        case SyncHazard::WRITE_AFTER_READ:
            return "WRITE_AFTER_READ";
            break;
        case SyncHazard::WRITE_AFTER_WRITE:
            return "WRITE_AFTER_WRITE";
            break;
        default:
            assert(0);
    }
    return "INVALID HAZARD";
}

template <typename Flags, typename Map>
SyncStageAccessFlags AccessScopeImpl(Flags flag_mask, const Map &map) {
    SyncStageAccessFlags scope = 0;
    for (const auto &bit_scope : map) {
        if (flag_mask < bit_scope.first) break;

        if (flag_mask & bit_scope.first) {
            scope |= bit_scope.second;
        }
    }
    return scope;
}

SyncStageAccessFlags SyncStageAccess::AccessScopeByStage(VkPipelineStageFlags stages) {
    return AccessScopeImpl(stages, syncStageAccessMaskByStageBit);
}

SyncStageAccessFlags SyncStageAccess::AccessScopeByAccess(VkAccessFlags accesses) {
    return AccessScopeImpl(accesses, syncStageAccessMaskByAccessBit);
}

// Getting from stage mask and access mask to stage/acess masks is something we need to be good at...
SyncStageAccessFlags SyncStageAccess::AccessScope(VkPipelineStageFlags stages, VkAccessFlags accesses) {
    // The access scope is the intersection of all stage/access types possible for the enabled stages and the enables accesses
    // (after doing a couple factoring of common terms the union of stage/access intersections is the intersections of the
    // union of all stage/access types for all the stages and the same unions for the access mask...
    return AccessScopeByStage(stages) & AccessScopeByAccess(accesses);
}

HazardResult ResourceAccessState::DetectHazard(SyncStageAccessIndex usage_index) const {
    HazardResult hazard;
    auto usage = FlagBit(usage_index);
    if (IsRead(usage) && IsWriteHazard(usage)) {
        hazard.Set(READ_AFTER_WRITE, write_tag);
    } else {
        // Assume write
        // TODO determine what to do with READ-WRITE usage states if any
        // Write-After-Write check -- if we have a previous write to test against
        if (last_write && IsWriteHazard(usage)) {
            hazard.Set(WRITE_AFTER_WRITE, write_tag);
        } else {
            // Only look for casus belli for WAR
            const auto usage_stage = PipelineStageBit(usage_index);
            for (uint32_t read_index = 0; read_index < last_read_count; read_index++) {
                if (IsReadHazard(usage_stage, last_reads[read_index])) {
                    hazard.Set(WRITE_AFTER_READ, last_reads[read_index].tag);
                    break;
                }
            }
        }
    }
    return hazard;
}

void ResourceAccessState::Update(SyncStageAccessIndex usage_index, const ResourceUsageTag &tag) {
    // Move this logic in the ResourceStateTracker as methods, thereof (or we'll repeat it for every flavor of resource...
    if (IsRead(usage_index)) {
        // Mulitple outstanding reads may be of interest and do dependency chains independently
        // However, for purposes of barrier tracking, only one read per pipeline stage matters
        const auto usage_bit = FlagBit(usage_index);
        const auto usage_stage = PipelineStageBit(usage_index);
        if (usage_stage & last_read_stages) {
            for (uint32_t read_index = 0; read_index < last_read_count; read_index++) {
                ReadState &access = last_reads[read_index];
                if (access.stage == usage_stage) {
                    access.barriers = 0;
                    access.tag = tag;
                    break;
                }
            }
        } else {
            // We don't have this stage in the list yet...
            assert(last_read_count < last_reads.size());
            ReadState &access = last_reads[last_read_count++];
            access.stage = usage_stage;
            access.barriers = 0;
            access.tag = tag;
            last_read_stages |= usage_stage;
        }
    } else {
        // Assume write
        // TODO determine what to do with READ-WRITE operations if any
        // Clobber last read and both sets of barriers... because all we have is DANGER, DANGER, WILL ROBINSON!!!
        // if the last_reads/last_write were unsafe, we've reported them,
        // in either case the prior access is irrelevant, we can overwrite them as *this* write is now after them
        last_read_count = 0;
        last_read_stages = 0;

        write_barriers = 0;
        write_tag = tag;
        last_write = FlagBit(usage_index);
    }
}
void ResourceAccessState::ApplyExecutionBarrier(VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask) {
    // Execution Barriers only protect read operations
    for (uint32_t read_index = 0; read_index < last_read_count; read_index++) {
        ReadState &access = last_reads[read_index];
        // The | implements the "dependency chain" logic for this access, as the barriers field stores the second sync scope
        if (srcStageMask & (access.stage | access.barriers)) {
            access.barriers |= dstStageMask;
        }
    }
}

void ResourceAccessState::ApplyMemoryBarrier(SyncStageAccessFlags src_scope, SyncStageAccessFlags dst_scope) {
    // Assuming we've applied the execution side of this barrier, we update just the write
    // The | implements the "dependency chain" logic for this access, as the barriers field stores the second access scope
    if (src_scope & (last_write | write_barriers)) {
        write_barriers |= dst_scope;
    }
}

void SyncValidator::ResetCommandBuffer(VkCommandBuffer command_buffer) {
    auto *tracker = GetAccessTrackerNoInsert(command_buffer);
    if (tracker) {
        tracker->Reset();
    }
}

void SyncValidator::ApplyGlobalBarriers(ResourceAccessTracker *tracker, VkPipelineStageFlags srcStageMask,
                                        VkPipelineStageFlags dstStageMask, SyncStageAccessFlags src_stage_scope,
                                        SyncStageAccessFlags dst_stage_scope, uint32_t memoryBarrierCount,
                                        const VkMemoryBarrier *pMemoryBarriers) {
    // TODO: Implement this better (maybe some delayed/on-demand integration).
    std::vector<std::pair<SyncStageAccessFlags, SyncStageAccessFlags>> barrier_scope;
    barrier_scope.reserve(memoryBarrierCount);

    // Don't want to create this per tracked item, but don't want to loop through all tracked items per barrier...
    for (uint32_t barrier_index = 0; barrier_index < memoryBarrierCount; barrier_index++) {
        const auto &barrier = pMemoryBarriers[barrier_index];
        barrier_scope.emplace_back(AccessScope(src_stage_scope, barrier.srcAccessMask),
                                   AccessScope(dst_stage_scope, barrier.dstAccessMask));
    }

    // First pass, just iterate over everything in the tracker... (yikes!)
    for (auto &tracked : tracker->map) {  // TODO hide the tracker details
        tracked.second.ApplyExecutionBarrier(srcStageMask, dstStageMask);
        for (uint32_t barrier_index = 0; barrier_index < memoryBarrierCount; barrier_index++) {
            const auto &scope = barrier_scope[barrier_index];
            tracked.second.ApplyMemoryBarrier(scope.first, scope.second);
        }
    }
}

void SyncValidator::ApplyBufferBarriers(ResourceAccessTracker *tracker, SyncStageAccessFlags src_stage_scope,
                                        SyncStageAccessFlags dst_stage_scope, uint32_t barrier_count,
                                        const VkBufferMemoryBarrier *barriers) {
    // TODO Implement this at subresource/memory_range accuracy
    for (uint32_t index = 0; index < barrier_count; index++) {
        const auto &barrier = barriers[index];
        auto *access_state = tracker->GetNoInsert(barrier.buffer);
        if (!access_state) continue;
        access_state->ApplyMemoryBarrier(AccessScope(src_stage_scope, barrier.srcAccessMask),
                                         AccessScope(dst_stage_scope, barrier.dstAccessMask));
    }
}

void SyncValidator::ApplyImageBarriers(ResourceAccessTracker *tracker, SyncStageAccessFlags src_stage_scope,
                                       SyncStageAccessFlags dst_stage_scope, uint32_t imageMemoryBarrierCount,
                                       const VkImageMemoryBarrier *pImageMemoryBarriers) {
    // TODO: Implement this. First pass a sub-resource (not-memory) accuracy
}

HazardResult SyncValidator::DetectHazard(const ResourceAccessTracker *tracker, SyncStageAccessIndex current_usage, VkBuffer buffer,
                                         const VkBufferCopy &region) const {
    // TODO -- region/mem-range accuracte detection
    // TODO this will have to be looped over the range of accesses within the range defined by "region"
    const auto access_state = tracker->Get(buffer);
    if (access_state) {
        return access_state->DetectHazard(current_usage);
    }
    return HazardResult();
}
void SyncValidator::UpdateAccessState(ResourceAccessTracker *tracker, SyncStageAccessIndex current_usage, VkBuffer buffer,
                                      const VkBufferCopy &region) {
    // TODO -- region/mem-range accuracte update
    auto access_state = tracker->Get(buffer);
    access_state->Update(current_usage, tag);
}

bool SyncValidator::PreCallValidateCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
                                                 uint32_t regionCount, const VkBufferCopy *pRegions) {
    bool skip = false;
    const auto *const const_this = this;
    const auto *tracker = const_this->GetAccessTracker(commandBuffer);
    if (tracker) {
        // If we have no previous accesses, we have no hazards
        // TODO: make this sub-resource capable
        // TODO: make this general, and stuff it into templates/utility functions
        const auto src_access = tracker->Get(srcBuffer);
        const auto dst_access = tracker->Get(dstBuffer);

        for (uint32_t region = 0; region < regionCount; region++) {
            auto hazard = DetectHazard(tracker, SYNC_TRANSFER_TRANSFER_READ, srcBuffer, pRegions[region]);
            if (hazard.hazard) {
                // TODO -- add tag information to log msg when useful.
                skip |= log_msg(report_data, VK_DEBUG_REPORT_ERROR_BIT_EXT, VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT,
                                HandleToUint64(srcBuffer), string_SyncHazardVUID(hazard.hazard),
                                "Hazard %s for srcBuffer %s, region %" PRIu32, string_SyncHazard(hazard.hazard),
                                report_data->FormatHandle(srcBuffer).c_str(), region);
            } else {
                hazard = DetectHazard(tracker, SYNC_TRANSFER_TRANSFER_WRITE, dstBuffer, pRegions[region]);
                if (hazard.hazard) {
                    skip |= log_msg(report_data, VK_DEBUG_REPORT_ERROR_BIT_EXT, VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT,
                                    HandleToUint64(dstBuffer), string_SyncHazardVUID(hazard.hazard),
                                    "Hazard %s for dstBuffer %s, region %" PRIu32, string_SyncHazard(hazard.hazard),
                                    report_data->FormatHandle(dstBuffer).c_str(), region);
                }
            }
            if (skip) break;
        }
    }
    return skip;
}

void SyncValidator::PreCallRecordCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
                                               uint32_t regionCount, const VkBufferCopy *pRegions) {
    auto *tracker = GetAccessTracker(commandBuffer);
    assert(tracker);
    for (uint32_t region = 0; region < regionCount; region++) {
        UpdateAccessState(tracker, SYNC_TRANSFER_TRANSFER_READ, srcBuffer, pRegions[region]);
        UpdateAccessState(tracker, SYNC_TRANSFER_TRANSFER_WRITE, dstBuffer, pRegions[region]);
    }
}

bool SyncValidator::PreCallValidateCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                                                      VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
                                                      uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
                                                      uint32_t bufferMemoryBarrierCount,
                                                      const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                                                      uint32_t imageMemoryBarrierCount,
                                                      const VkImageMemoryBarrier *pImageMemoryBarriers) {
    bool skip = false;

    return skip;
}

void SyncValidator::PreCallRecordCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                                                    VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
                                                    uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
                                                    uint32_t bufferMemoryBarrierCount,
                                                    const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                                                    uint32_t imageMemoryBarrierCount,
                                                    const VkImageMemoryBarrier *pImageMemoryBarriers) {
    // Just implement the buffer barrier for now
    auto *tracker = GetAccessTracker(commandBuffer);
    assert(tracker);
    auto src_stage_scope = AccessScopeByStage(srcStageMask);
    auto dst_stage_scope = AccessScopeByStage(dstStageMask);

    ApplyBufferBarriers(tracker, src_stage_scope, dst_stage_scope, bufferMemoryBarrierCount, pBufferMemoryBarriers);
    ApplyImageBarriers(tracker, src_stage_scope, dst_stage_scope, imageMemoryBarrierCount, pImageMemoryBarriers);

    // Apply these last in-case there operation is a superset of the other two and would clean them up...
    ApplyGlobalBarriers(tracker, srcStageMask, dstStageMask, src_stage_scope, dst_stage_scope, memoryBarrierCount, pMemoryBarriers);
}

void SyncValidator::PostCallRecordCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                               const VkAllocationCallbacks *pAllocator, VkDevice *pDevice, VkResult result) {
    // The state tracker sets up the device state
    StateTracker::PostCallRecordCreateDevice(gpu, pCreateInfo, pAllocator, pDevice, result);

    // Add the callback hooks for the functions that are either broadly or deeply used and that the ValidationStateTracker refactor
    // would be messier without.
    // TODO: Find a good way to do this hooklessly.
    ValidationObject *device_object = GetLayerDataPtr(get_dispatch_key(*pDevice), layer_data_map);
    ValidationObject *validation_data = GetValidationObject(device_object->object_dispatch, LayerObjectTypeSyncValidation);
    SyncValidator *sync_device_state = static_cast<SyncValidator *>(validation_data);

    sync_device_state->SetCommandBufferResetCallback(
        [sync_device_state](VkCommandBuffer command_buffer) -> void { sync_device_state->ResetCommandBuffer(command_buffer); });
}
