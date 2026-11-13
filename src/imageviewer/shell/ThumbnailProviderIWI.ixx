module;

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objidl.h>
#include <thumbcache.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

export module ThumbnailProviderIWI;

import IwiThumbnail;
import ThumbnailProviderIds;

/*
 * ThumbnailProviderIWI — the Windows shell thumbnail handler for .iwi.
 *
 * This module is only the COM wrapper.  Everything that decides what a
 * thumbnail looks like is in IwiThumbnail, which is portable and unit-tested;
 * here we hand the shell's IStream to that code, turn the resulting BGRA raster
 * into an HBITMAP with CreateDIBSection, and make sure no C++ exception ever
 * crosses a COM boundary.
 *
 * # Why the provider does not load ImageLoaderIWI.dll
 *
 * The loader is a crash-isolated plugin loaded by iview.exe.  A shell extension
 * runs inside dllhost.exe, where loading a second, mutable DLL buys nothing:
 * the two would still have to agree about the format, and the provider needs a
 * different thing from the file anyway (a decimated raster, read row-wise).
 * Importing IwiFormat instead gives one statement of the accepted files at a
 * fraction of the memory: a 4096x4096 IWI is never fully allocated here.
 *
 * # Registration
 *
 * Deliberately no DllRegisterServer.  regsvr32 runs elevated, and a thumbnail
 * handler registered for the administrator is registered for the wrong user;
 * the association scripts write the per-user keys instead (HKCU, no UAC).  This
 * DLL therefore exports only the two functions COM itself calls.
 */

namespace {

LONG g_objectCount = 0;
LONG g_serverLock  = 0;

/*
 * The shell hands us an IStream opened read-only.  Reads are positional, so the
 * source seeks for each row it wants rather than reading the file front to
 * back: a thumbnail of a large texture touches a few hundred rows, not all of
 * them.
 */
class StreamSource final : public iwi::SampleSource {
public:
    explicit StreamSource(IStream* stream) : stream_(stream)
    {
        STATSTG stat{};
        if (stream_ != nullptr &&
            SUCCEEDED(stream_->Stat(&stat, STATFLAG_NONAME))) {
            size_      = stat.cbSize.QuadPart;
            sizeKnown_ = true;
            return;
        }
        /* Not every stream implements Stat; seeking to the end is equivalent
         * and is what the file-backed streams Explorer uses always support. */
        if (stream_ != nullptr) {
            LARGE_INTEGER zero{};
            ULARGE_INTEGER end{};
            if (SUCCEEDED(stream_->Seek(zero, STREAM_SEEK_END, &end))) {
                size_      = end.QuadPart;
                sizeKnown_ = true;
                stream_->Seek(zero, STREAM_SEEK_SET, nullptr);
            }
        }
    }

    uint64_t Size() const override { return size_; }

    bool ReadAt(uint64_t offset, void* dst, std::size_t bytes) override
    {
        if (stream_ == nullptr || !sizeKnown_) return false;
        if (offset > size_ || bytes > size_ - offset) return false;

        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (FAILED(stream_->Seek(position, STREAM_SEEK_SET, nullptr)))
            return false;

        auto* out = static_cast<BYTE*>(dst);
        std::size_t remaining = bytes;
        while (remaining > 0) {
            const ULONG chunk = static_cast<ULONG>(
                std::min<std::size_t>(remaining, 1u << 20));
            ULONG read = 0;
            if (FAILED(stream_->Read(out, chunk, &read)) || read != chunk)
                return false;
            out       += chunk;
            remaining -= chunk;
        }
        return true;
    }

private:
    IStream* stream_    = nullptr;
    uint64_t size_      = 0;
    bool     sizeKnown_ = false;
};

/*
 * The object COM creates.  Implements the two interfaces a thumbnail handler
 * needs: IInitializeWithStream (so the shell can hand us the file) and
 * IThumbnailProvider (so it can ask for the bitmap).  Nothing is cached between
 * calls, so the object is safe on any apartment thread.
 */
class IwiThumbnailProvider final : public IThumbnailProvider,
                                   public IInitializeWithStream {
public:
    IwiThumbnailProvider() { InterlockedIncrement(&g_objectCount); }

    IwiThumbnailProvider(const IwiThumbnailProvider&) = delete;
    IwiThumbnailProvider& operator=(const IwiThumbnailProvider&) = delete;

    /* IUnknown */
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;

        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, iwthumb::kIID_IThumbnailProvider)) {
            *ppv = static_cast<IThumbnailProvider*>(this);
        } else if (IsEqualIID(riid, IID_IInitializeWithStream)) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const LONG remaining = InterlockedDecrement(&references_);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    /* IInitializeWithStream */
    IFACEMETHODIMP Initialize(IStream* stream, DWORD /*grfMode*/) override
    {
        if (stream == nullptr) return E_INVALIDARG;
        if (stream_ != nullptr) return E_UNEXPECTED;   /* already initialised */
        stream_ = stream;
        stream_->AddRef();
        return S_OK;
    }

    /* IThumbnailProvider */
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp,
                                WTS_ALPHATYPE* pdwAlpha) override
    {
        if (phbmp == nullptr || pdwAlpha == nullptr) return E_POINTER;
        *phbmp    = nullptr;
        *pdwAlpha = WTSAT_UNKNOWN;
        if (stream_ == nullptr) return E_UNEXPECTED;

        StreamSource source(stream_);

        iwi::ThumbnailImage image;
        std::string reason;
        const iwi::ThumbnailStatus status =
            iwi::RenderThumbnail(source, static_cast<uint32_t>(cx),
                                 image, &reason);

        /* IoError is transient (the file did not read); everything else means
         * this is not an IWI Explorer can show.  Both are E_FAIL: the shell
         * then draws the file-type icon, which is the intended fallback. */
        if (status != iwi::ThumbnailStatus::Ok)
            return E_FAIL;
        if (image.width == 0 || image.height == 0 ||
            image.bgra.size() != static_cast<std::size_t>(image.width) *
                                 image.height * 4u)
            return E_FAIL;

        BITMAPINFO info{};
        info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth       = static_cast<LONG>(image.width);
        /* Negative height asks for a top-down DIB, which is the row order
         * RenderThumbnail produces. */
        info.bmiHeader.biHeight      = -static_cast<LONG>(image.height);
        info.bmiHeader.biPlanes      = 1;
        info.bmiHeader.biBitCount    = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                          &bits, nullptr, 0);
        if (bitmap == nullptr) return E_OUTOFMEMORY;
        if (bits == nullptr) {
            DeleteObject(bitmap);
            return E_OUTOFMEMORY;
        }

        std::memcpy(bits, image.bgra.data(), image.bgra.size());

        *phbmp    = bitmap;
        *pdwAlpha = image.hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
        return S_OK;
    }

protected:
    ~IwiThumbnailProvider()
    {
        if (stream_ != nullptr) stream_->Release();
        InterlockedDecrement(&g_objectCount);
    }

private:
    LONG     references_ = 1;
    IStream* stream_     = nullptr;
};

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() { InterlockedIncrement(&g_serverLock); }

    ClassFactory(const ClassFactory&) = delete;
    ClassFactory& operator=(const ClassFactory&) = delete;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (!IsEqualIID(riid, IID_IUnknown) && !IsEqualIID(riid, IID_IClassFactory))
            return E_NOINTERFACE;
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const LONG remaining = InterlockedDecrement(&references_);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                                  void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (outer != nullptr) return CLASS_E_NOAGGREGATION;

        auto* provider = new (std::nothrow) IwiThumbnailProvider();
        if (provider == nullptr) return E_OUTOFMEMORY;

        const HRESULT hr = provider->QueryInterface(riid, ppv);
        provider->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) InterlockedIncrement(&g_serverLock);
        else      InterlockedDecrement(&g_serverLock);
        return S_OK;
    }

protected:
    ~ClassFactory() { InterlockedDecrement(&g_serverLock); }

private:
    LONG references_ = 1;
};

} // namespace

/*
 * The COM entry points.  Spelled with STDAPI — exactly the declaration
 * combaseapi.h already made for them — so the definition is a definition and
 * not a conflicting redeclaration.  They are exported by
 * ThumbnailProviderIWI.def; see the comment in that file for why.
 */
STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(clsid, iwthumb::CLSID_IwiThumbnailProvider))
        return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) ClassFactory();
    if (factory == nullptr) return E_OUTOFMEMORY;

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_objectCount == 0 && g_serverLock == 0) ? S_OK : S_FALSE;
}

/*
 * Not exported: the CRT's entry point calls it by name, which is the only
 * caller.  Its whole job is to skip per-thread notifications this DLL has no
 * use for.
 */
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
