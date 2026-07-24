if (-not ("Yolo11.Acceptance.NativeProcessTree" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Yolo11.Acceptance
{
    public static class NativeProcessTree
    {
        private const uint TH32CS_SNAPPROCESS = 0x00000002;
        private static readonly IntPtr InvalidHandle = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct PROCESSENTRY32
        {
            public uint dwSize;
            public uint cntUsage;
            public uint th32ProcessID;
            public IntPtr th32DefaultHeapID;
            public uint th32ModuleID;
            public uint cntThreads;
            public uint th32ParentProcessID;
            public int pcPriClassBase;
            public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string szExeFile;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateToolhelp32Snapshot(
            uint flags,
            uint processId);

        [DllImport(
            "kernel32.dll",
            CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern bool Process32FirstW(
            IntPtr snapshot,
            ref PROCESSENTRY32 entry);

        [DllImport(
            "kernel32.dll",
            CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern bool Process32NextW(
            IntPtr snapshot,
            ref PROCESSENTRY32 entry);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);

        public static int[] ChildProcessIds(
            int parentProcessId,
            string executableName)
        {
            IntPtr snapshot = CreateToolhelp32Snapshot(
                TH32CS_SNAPPROCESS,
                0);
            if (snapshot == InvalidHandle)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            try
            {
                var children = new List<int>();
                var entry = new PROCESSENTRY32();
                entry.dwSize = (uint)Marshal.SizeOf(entry);
                if (!Process32FirstW(snapshot, ref entry))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 18)
                    {
                        return children.ToArray();
                    }
                    throw new Win32Exception(error);
                }
                do
                {
                    if (entry.th32ParentProcessID ==
                            (uint)parentProcessId &&
                        string.Equals(
                            entry.szExeFile,
                            executableName,
                            StringComparison.OrdinalIgnoreCase))
                    {
                        children.Add((int)entry.th32ProcessID);
                    }
                    entry.dwSize = (uint)Marshal.SizeOf(entry);
                }
                while (Process32NextW(snapshot, ref entry));
                return children.ToArray();
            }
            finally
            {
                CloseHandle(snapshot);
            }
        }
    }
}
'@
}

function Get-NativeChildProcessId {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [int]$ParentProcessId,
        [Parameter(Mandatory = $true)]
        [string]$ExecutableName
    )
    return @(
        [Yolo11.Acceptance.NativeProcessTree]::ChildProcessIds(
            $ParentProcessId,
            $ExecutableName)
    )
}
