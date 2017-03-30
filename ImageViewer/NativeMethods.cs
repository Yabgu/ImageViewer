using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace ImageViewer
{
    static class NativeMethods
    {
        [DllImport("kernel32.dll", EntryPoint = "CopyMemory", SetLastError = false)]
        public static extern void CopyMemory(IntPtr dest, IntPtr src, uint count);
        [DllImport("kernel32.dll")]
        public static extern void RtlZeroMemory(IntPtr dst, int length);
    }
}
