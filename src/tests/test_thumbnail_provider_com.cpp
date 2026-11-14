/*
 * test_thumbnail_provider_com.cpp — the server lifetime contract of
 * IwiThumbnailProvider.dll, driven through its exported entry points.
 *
 * # Why this exists
 *
 * DllCanUnloadNow is the one part of a COM server that a wrong answer cannot
 * be caught by: it does not fail, it either lets the loader free a DLL that is
 * still in use or keeps one loaded for ever.  Both failures are invisible in
 * an ordinary thumbnail test, which is why the states are asserted here one at
 * a time.
 *
 * The rule being pinned: DllCanUnloadNow may return S_OK only when no object
 * and no outstanding class factory exist.  A class factory pins the module for
 * as long as a client holds it, because the client's pointer points at a
 * vtable inside this DLL — reporting "unloadable" while it is held is a
 * use-after-free the moment the client calls CreateInstance.
 *
 * This is the same shape as ATL's DLL-server class factory (atlcom.h,
 * CComObjectCached): AddRef to 2 calls _pAtlModule->Lock(); Release to 0 calls
 * Unlock.  LockServer is an additional, client-driven pin on top of that, not a
 * replacement for it.
 *
 * The cases below therefore assert:
 *   - a freshly loaded module is unloadable;
 *   - it is NOT unloadable while a class factory is held, and IS again once
 *     the factory is released;
 *   - it is NOT unloadable while an object is held, and IS again afterwards;
 *   - LockServer(TRUE/FALSE) adds and removes a pin independently;
 *   - a factory and an object held together, then released in either order,
 *     leave the module unloadable — i.e. nothing leaks a pin.
 */

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
/* IUnknown, IClassFactory and IID_IClassFactory.  WIN32_LEAN_AND_MEAN above
 * leaves the OLE headers out, so they are included explicitly. */
#include <objbase.h>
#include <unknwn.h>

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

import ThumbnailProviderIds;

#ifndef IW_THUMBNAIL_PROVIDER_DLL
#  error "IW_THUMBNAIL_PROVIDER_DLL must be defined by the build system"
#endif

namespace {

using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
using DllCanUnloadNowFn   = HRESULT(WINAPI*)();

/*
 * The DLL, loaded the way the shell's surrogate loads it: by path, with the
 * three entry points resolved by name.  Nothing here uses CoCreateInstance,
 * because the registration is not required for this contract — only the
 * exported functions are.
 */
class ProviderModule {
public:
    ProviderModule()
    {
        /* The path comes from the build system as a narrow string; converting
         * once and keeping it alive for the call is all this needs. */
        const std::wstring path = ToWide(IW_THUMBNAIL_PROVIDER_DLL);
        handle_ = LoadLibraryW(path.c_str());
    }

    ~ProviderModule()
    {
        if (handle_ != nullptr)
            FreeLibrary(handle_);
    }

    ProviderModule(const ProviderModule&) = delete;
    ProviderModule& operator=(const ProviderModule&) = delete;

    bool loaded() const { return handle_ != nullptr; }

    DllGetClassObjectFn getClassObject() const
    {
        return reinterpret_cast<DllGetClassObjectFn>(
            GetProcAddress(handle_, "DllGetClassObject"));
    }

    DllCanUnloadNowFn canUnloadNow() const
    {
        return reinterpret_cast<DllCanUnloadNowFn>(
            GetProcAddress(handle_, "DllCanUnloadNow"));
    }

private:
    static std::wstring ToWide(const char* text)
    {
        const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(size > 0 ? size - 1 : 0),
                          L'\0');
        if (size > 1) {
            MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), size);
        }
        return wide;
    }

    HMODULE handle_ = nullptr;
};

/* A factory and/or object held for the duration of a case. */
struct Held {
    IClassFactory* factory = nullptr;
    IUnknown*      object  = nullptr;

    ~Held()
    {
        if (object  != nullptr) object->Release();
        if (factory != nullptr) factory->Release();
    }
};

ProviderModule& Module()
{
    static ProviderModule module;
    return module;
}

} // namespace

TEST_SUITE("IWI thumbnail provider: COM lifetime")
{
    TEST_CASE("The provider DLL loads and exports both COM entry points")
    {
        /* A missing export means the .def and the module disagreed, which is
         * how the provider silently stops being a COM server at all. */
        REQUIRE(Module().loaded());
        CHECK(Module().getClassObject() != nullptr);
        CHECK(Module().canUnloadNow() != nullptr);
    }

    TEST_CASE("A freshly loaded module reports itself unloadable")
    {
        REQUIRE(Module().loaded());
        CHECK(Module().canUnloadNow()() == S_OK);
    }

    TEST_CASE("Holding a class factory pins the module until it is released")
    {
        REQUIRE(Module().loaded());

        Held held;
        REQUIRE(Module().getClassObject()(
                    iwthumb::CLSID_IwiThumbnailProvider, IID_IClassFactory,
                    reinterpret_cast<void**>(&held.factory)) == S_OK);
        REQUIRE(held.factory != nullptr);

        /* The client now holds a pointer into this DLL.  Reporting S_OK here
         * would let the loader free the code behind that pointer. */
        CHECK(Module().canUnloadNow()() == S_FALSE);

        held.factory->Release();
        held.factory = nullptr;

        /* Back to unloadable: the factory's pin is not leaked. */
        CHECK(Module().canUnloadNow()() == S_OK);
    }

    TEST_CASE("Holding an object pins the module until it is released")
    {
        REQUIRE(Module().loaded());

        Held held;
        REQUIRE(Module().getClassObject()(
                    iwthumb::CLSID_IwiThumbnailProvider, IID_IClassFactory,
                    reinterpret_cast<void**>(&held.factory)) == S_OK);
        REQUIRE(held.factory->CreateInstance(
                    nullptr, IID_IUnknown,
                    reinterpret_cast<void**>(&held.object)) == S_OK);
        REQUIRE(held.object != nullptr);

        CHECK(Module().canUnloadNow()() == S_FALSE);

        held.object->Release();
        held.object = nullptr;
        CHECK(Module().canUnloadNow()() == S_FALSE);   /* factory still held */

        held.factory->Release();
        held.factory = nullptr;
        CHECK(Module().canUnloadNow()() == S_OK);
    }

    TEST_CASE("LockServer pins and unpins independently of any object")
    {
        REQUIRE(Module().loaded());

        Held held;
        REQUIRE(Module().getClassObject()(
                    iwthumb::CLSID_IwiThumbnailProvider, IID_IClassFactory,
                    reinterpret_cast<void**>(&held.factory)) == S_OK);

        CHECK(held.factory->LockServer(TRUE) == S_OK);
        CHECK(Module().canUnloadNow()() == S_FALSE);

        /* Releasing the factory must not clear a client's explicit pin. */
        held.factory->Release();
        held.factory = nullptr;
        CHECK(Module().canUnloadNow()() == S_FALSE);

        /* Re-acquire a factory purely to call LockServer(FALSE) — the standard
         * way a client removes its pin, since LockServer lives on a factory. */
        REQUIRE(Module().getClassObject()(
                    iwthumb::CLSID_IwiThumbnailProvider, IID_IClassFactory,
                    reinterpret_cast<void**>(&held.factory)) == S_OK);
        CHECK(held.factory->LockServer(FALSE) == S_OK);
        held.factory->Release();
        held.factory = nullptr;

        CHECK(Module().canUnloadNow()() == S_OK);
    }

    TEST_CASE("Factory and object released in either order leak no pin")
    {
        REQUIRE(Module().loaded());

        for (int order = 0; order < 2; ++order) {
            Held held;
            REQUIRE(Module().getClassObject()(
                        iwthumb::CLSID_IwiThumbnailProvider, IID_IClassFactory,
                        reinterpret_cast<void**>(&held.factory)) == S_OK);
            REQUIRE(held.factory->CreateInstance(
                        nullptr, IID_IUnknown,
                        reinterpret_cast<void**>(&held.object)) == S_OK);
            CHECK(Module().canUnloadNow()() == S_FALSE);

            if (order == 0) {              /* factory first, then object */
                held.factory->Release();
                held.factory = nullptr;
                held.object->Release();
                held.object = nullptr;
            } else {                       /* object first, then factory */
                held.object->Release();
                held.object = nullptr;
                held.factory->Release();
                held.factory = nullptr;
            }

            INFO("release order " << order);
            CHECK(Module().canUnloadNow()() == S_OK);
        }
    }

    TEST_CASE("A foreign CLSID is refused, and refuses nothing into the caller")
    {
        REQUIRE(Module().loaded());

        const CLSID unknown = { 0x11111111, 0x2222, 0x3333,
                                { 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB } };
        void* out = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));

        CHECK(Module().getClassObject()(unknown, IID_IClassFactory, &out) ==
              CLASS_E_CLASSNOTAVAILABLE);
        /* The out-parameter must be cleared, not left holding the sentinel. */
        CHECK(out == nullptr);
        CHECK(Module().canUnloadNow()() == S_OK);
    }

    TEST_CASE("A failed QueryInterface leaves no factory or object behind")
    {
        REQUIRE(Module().loaded());

        Held held;
        /* An IID the provider does not implement. */
        const IID unknown = { 0xAAAAAAAA, 0xBBBB, 0xCCCC,
                              { 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33, 0x44 } };
        void* out = nullptr;

        CHECK(Module().getClassObject()(iwthumb::CLSID_IwiThumbnailProvider,
                                        unknown, &out) == E_NOINTERFACE);
        CHECK(out == nullptr);
        CHECK(Module().canUnloadNow()() == S_OK);
    }
}
