#include <windows.h>
#include <minwindef.h>
#include <winnt.h>
#include <shellapi.h>
#include <winuser.h>

#include "main_generic.hpp"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    LPWSTR* argv;
    int argc;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr)
    {
        argc = 0;
    }

    const int result = real_main(argc, argv);

    if (argv != nullptr)
    {
        LocalFree(argv);
    }

    return result;
}
