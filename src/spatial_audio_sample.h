#pragma once

#include <mfspatialaudio.h>
#include <SpatialAudioMetadata.h>

namespace dolby {

HRESULT CreateSpatialAudioSample(IMFSpatialAudioSample** sample);
HRESULT AddSpatialAudioObjectBuffers(IMFSpatialAudioSample* sample,
                                     ISpatialAudioMetadataClient* metadataClient,
                                     UINT32 objectCount, UINT32 frameCount,
                                     UINT32 maxMetadataItems);

} // namespace dolby
