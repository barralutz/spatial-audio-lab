#include "spatial_audio_sample.h"

#include <mfapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <new>
#include <vector>

namespace dolby {
namespace {

using Microsoft::WRL::ComPtr;

class SpatialAudioObjectBuffer final : public IMFSpatialAudioObjectBuffer {
public:
    SpatialAudioObjectBuffer(IMFMediaBuffer* buffer, ISpatialAudioMetadataItems* metadata)
        : buffer_(buffer), metadata_(metadata) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IMFMediaBuffer ||
            interfaceId == IID_IMFSpatialAudioObjectBuffer) {
            *object = static_cast<IMFSpatialAudioObjectBuffer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Lock(BYTE** data, DWORD* maximum, DWORD* current) override {
        return buffer_->Lock(data, maximum, current);
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { return buffer_->Unlock(); }
    HRESULT STDMETHODCALLTYPE GetCurrentLength(DWORD* length) override {
        return buffer_->GetCurrentLength(length);
    }
    HRESULT STDMETHODCALLTYPE SetCurrentLength(DWORD length) override {
        return buffer_->SetCurrentLength(length);
    }
    HRESULT STDMETHODCALLTYPE GetMaxLength(DWORD* length) override {
        return buffer_->GetMaxLength(length);
    }

    HRESULT STDMETHODCALLTYPE SetID(const UINT32 id) override {
        id_ = id;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetID(UINT32* id) override {
        if (id == nullptr) return E_POINTER;
        *id = id_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetType(const AudioObjectType type) override {
        type_ = type;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetType(AudioObjectType* type) override {
        if (type == nullptr) return E_POINTER;
        *type = type_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetadataItems(ISpatialAudioMetadataItems** metadata) override {
        if (metadata == nullptr) return E_POINTER;
        *metadata = metadata_.Get();
        if (*metadata == nullptr) return E_UNEXPECTED;
        (*metadata)->AddRef();
        return S_OK;
    }

private:
    ~SpatialAudioObjectBuffer() = default;

    std::atomic<ULONG> references_{1};
    ComPtr<IMFMediaBuffer> buffer_;
    ComPtr<ISpatialAudioMetadataItems> metadata_;
    UINT32 id_{0xffff'ffff};
    AudioObjectType type_{AudioObjectType_None};
};

class SpatialAudioSample final : public IMFSpatialAudioSample {
public:
    explicit SpatialAudioSample(IMFSample* sample) : sample_(sample) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IMFAttributes ||
            interfaceId == IID_IMFSample || interfaceId == IID_IMFSpatialAudioSample) {
            *object = static_cast<IMFSpatialAudioSample*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

#define FORWARD(method, declaration, arguments) \
    HRESULT STDMETHODCALLTYPE method declaration override { return sample_->method arguments; }

    FORWARD(GetItem, (REFGUID key, PROPVARIANT* value), (key, value))
    FORWARD(GetItemType, (REFGUID key, MF_ATTRIBUTE_TYPE* type), (key, type))
    FORWARD(CompareItem, (REFGUID key, REFPROPVARIANT value, BOOL* result),
            (key, value, result))
    FORWARD(Compare, (IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE matchType, BOOL* result),
            (theirs, matchType, result))
    FORWARD(GetUINT32, (REFGUID key, UINT32* value), (key, value))
    FORWARD(GetUINT64, (REFGUID key, UINT64* value), (key, value))
    FORWARD(GetDouble, (REFGUID key, double* value), (key, value))
    FORWARD(GetGUID, (REFGUID key, GUID* value), (key, value))
    FORWARD(GetStringLength, (REFGUID key, UINT32* length), (key, length))
    FORWARD(GetString, (REFGUID key, LPWSTR value, UINT32 size, UINT32* length),
            (key, value, size, length))
    FORWARD(GetAllocatedString, (REFGUID key, LPWSTR* value, UINT32* length),
            (key, value, length))
    FORWARD(GetBlobSize, (REFGUID key, UINT32* size), (key, size))
    FORWARD(GetBlob, (REFGUID key, UINT8* buffer, UINT32 size, UINT32* blobSize),
            (key, buffer, size, blobSize))
    FORWARD(GetAllocatedBlob, (REFGUID key, UINT8** buffer, UINT32* size),
            (key, buffer, size))
    FORWARD(GetUnknown, (REFGUID key, REFIID interfaceId, LPVOID* object),
            (key, interfaceId, object))
    FORWARD(SetItem, (REFGUID key, REFPROPVARIANT value), (key, value))
    FORWARD(DeleteItem, (REFGUID key), (key))
    FORWARD(DeleteAllItems, (), ())
    FORWARD(SetUINT32, (REFGUID key, UINT32 value), (key, value))
    FORWARD(SetUINT64, (REFGUID key, UINT64 value), (key, value))
    FORWARD(SetDouble, (REFGUID key, double value), (key, value))
    FORWARD(SetGUID, (REFGUID key, REFGUID value), (key, value))
    FORWARD(SetString, (REFGUID key, LPCWSTR value), (key, value))
    FORWARD(SetBlob, (REFGUID key, const UINT8* buffer, UINT32 size),
            (key, buffer, size))
    FORWARD(SetUnknown, (REFGUID key, IUnknown* object), (key, object))
    FORWARD(LockStore, (), ())
    FORWARD(UnlockStore, (), ())
    FORWARD(GetCount, (UINT32* count), (count))
    FORWARD(GetItemByIndex, (UINT32 index, GUID* key, PROPVARIANT* value),
            (index, key, value))
    FORWARD(CopyAllItems, (IMFAttributes* destination), (destination))

    FORWARD(GetSampleFlags, (DWORD* flags), (flags))
    FORWARD(SetSampleFlags, (DWORD flags), (flags))
    FORWARD(GetSampleTime, (LONGLONG* time), (time))
    FORWARD(SetSampleTime, (LONGLONG time), (time))
    FORWARD(GetSampleDuration, (LONGLONG* duration), (duration))
    FORWARD(SetSampleDuration, (LONGLONG duration), (duration))
    FORWARD(GetBufferCount, (DWORD* count), (count))
    FORWARD(GetBufferByIndex, (DWORD index, IMFMediaBuffer** buffer), (index, buffer))
    FORWARD(ConvertToContiguousBuffer, (IMFMediaBuffer** buffer), (buffer))
    FORWARD(GetTotalLength, (DWORD* length), (length))
    FORWARD(CopyToBuffer, (IMFMediaBuffer* buffer), (buffer))

#undef FORWARD

    HRESULT STDMETHODCALLTYPE AddBuffer(IMFMediaBuffer* buffer) override {
        if (buffer == nullptr) return E_POINTER;
        const HRESULT result = sample_->AddBuffer(buffer);
        if (FAILED(result)) return result;

        ComPtr<IMFSpatialAudioObjectBuffer> spatialObject;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&spatialObject)))) {
            AddObjectReference(spatialObject.Get());
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RemoveBufferByIndex(const DWORD index) override {
        ComPtr<IMFMediaBuffer> buffer;
        const HRESULT lookup = sample_->GetBufferByIndex(index, &buffer);
        if (FAILED(lookup)) return lookup;
        const HRESULT result = sample_->RemoveBufferByIndex(index);
        if (FAILED(result)) return result;

        ComPtr<IMFSpatialAudioObjectBuffer> spatialObject;
        if (SUCCEEDED(buffer.As(&spatialObject))) RemoveObjectReference(spatialObject.Get());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RemoveAllBuffers() override {
        const HRESULT result = sample_->RemoveAllBuffers();
        if (SUCCEEDED(result)) objects_.clear();
        return result;
    }

    HRESULT STDMETHODCALLTYPE GetObjectCount(DWORD* count) override {
        if (count == nullptr) return E_POINTER;
        *count = static_cast<DWORD>(objects_.size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE AddSpatialAudioObject(
        IMFSpatialAudioObjectBuffer* object) override {
        if (object == nullptr) return E_POINTER;
        if (ContainsObject(object)) return S_OK;
        const HRESULT result = sample_->AddBuffer(object);
        if (FAILED(result)) return result;
        objects_.emplace_back(object);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSpatialAudioObjectByIndex(
        const DWORD index, IMFSpatialAudioObjectBuffer** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (index >= objects_.size()) return E_INVALIDARG;
        *object = objects_[index].Get();
        (*object)->AddRef();
        return S_OK;
    }

private:
    ~SpatialAudioSample() = default;

    bool ContainsObject(IMFSpatialAudioObjectBuffer* object) const {
        return std::any_of(objects_.begin(), objects_.end(), [object](const auto& candidate) {
            return candidate.Get() == object;
        });
    }

    void AddObjectReference(IMFSpatialAudioObjectBuffer* object) {
        if (!ContainsObject(object)) objects_.emplace_back(object);
    }

    void RemoveObjectReference(IMFSpatialAudioObjectBuffer* object) {
        const auto match = std::find_if(objects_.begin(), objects_.end(),
                                        [object](const auto& candidate) {
                                            return candidate.Get() == object;
                                        });
        if (match != objects_.end()) objects_.erase(match);
    }

    std::atomic<ULONG> references_{1};
    ComPtr<IMFSample> sample_;
    std::vector<ComPtr<IMFSpatialAudioObjectBuffer>> objects_;
};

} // namespace

HRESULT CreateSpatialAudioSample(IMFSpatialAudioSample** sample) {
    if (sample == nullptr) return E_POINTER;
    *sample = nullptr;

    ComPtr<IMFSample> baseSample;
    const HRESULT result = MFCreateSample(&baseSample);
    if (FAILED(result)) return result;

    auto* spatialSample = new (std::nothrow) SpatialAudioSample(baseSample.Get());
    if (spatialSample == nullptr) return E_OUTOFMEMORY;
    *sample = spatialSample;
    return S_OK;
}

HRESULT AddSpatialAudioObjectBuffers(IMFSpatialAudioSample* sample,
                                     ISpatialAudioMetadataClient* metadataClient,
                                     const UINT32 objectCount, const UINT32 frameCount,
                                     const UINT32 maxMetadataItems) {
    if (sample == nullptr || metadataClient == nullptr) return E_POINTER;
    if (objectCount == 0 || objectCount > 64 || frameCount == 0 || frameCount > 0xffff ||
        maxMetadataItems > 0xffff) {
        return E_INVALIDARG;
    }

    const UINT64 audioBytes = static_cast<UINT64>(frameCount) * sizeof(float);
    if (audioBytes > MAXDWORD) return E_INVALIDARG;
    for (UINT32 index = 0; index < objectCount; ++index) {
        ComPtr<IMFMediaBuffer> audioBuffer;
        HRESULT result = MFCreateMemoryBuffer(static_cast<DWORD>(audioBytes), &audioBuffer);
        if (FAILED(result)) return result;

        ComPtr<ISpatialAudioMetadataItems> metadata;
        result = metadataClient->ActivateSpatialAudioMetadataItems(
            static_cast<UINT16>(maxMetadataItems), static_cast<UINT16>(frameCount), nullptr,
            &metadata);
        if (FAILED(result)) return result;

        auto* rawObject = new (std::nothrow)
            SpatialAudioObjectBuffer(audioBuffer.Get(), metadata.Get());
        if (rawObject == nullptr) return E_OUTOFMEMORY;
        ComPtr<IMFSpatialAudioObjectBuffer> object;
        object.Attach(rawObject);
        result = sample->AddSpatialAudioObject(object.Get());
        if (FAILED(result)) return result;
    }
    return S_OK;
}

} // namespace dolby
