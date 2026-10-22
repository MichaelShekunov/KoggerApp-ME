#requires -Version 5.1
<#
.SYNOPSIS
    Runs the app under test on a desktop of its own, out of the way of whoever works at the machine.

.DESCRIPTION
    Dot-source it: . (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')

    The owner, 2026-10-22: the checks' app windows came up over his work and took the focus. The app brings
    its window to the front at start (main.cpp bringWindowToFront: AttachThreadInput to the foreground
    window's thread, the foreground lock off, SetForegroundWindow), so a check that started it stole the
    keyboard from whatever the owner was typing in. Here the app starts on a second desktop of the
    interactive window station (CreateDesktop, never switched to): it runs, draws and grabs its window
    there as usual, but nothing of it shows on the owner's desktop, and Windows passes no window message
    and no thread-input attachment between desktops, so it cannot take the owner's focus either.

    A check that talks to the app's window (wheel, resize, drag) does it from a thread of its own on that
    desktop: Invoke-OnTestDesktop.

    Start-TestDesktopProcess   in place of [System.Diagnostics.Process]::Start($psi)
    Get-TestDesktopWindow      in place of $process.MainWindowHandle (that one only sees this desktop)
    Invoke-OnTestDesktop       a static method run on the test desktop's thread
    Send-TestDesktopWheel      wheel notches posted to a point of the app's window
#>

if (-not ('KoggerTestDesktop' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class KoggerTestDesktop
{
    public const string DesktopName = "KoggerChecks";

    static IntPtr desktop_ = IntPtr.Zero;
    static Thread worker_;
    static BlockingCollection<Action> queue_;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; }

    delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr devmode, uint flags, uint access, IntPtr sa);
    [DllImport("user32.dll", SetLastError = true)] static extern bool SetThreadDesktop(IntPtr desktop);
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr GetProcessWindowStation();
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool GetUserObjectInformationW(IntPtr obj, int index, IntPtr info, uint length, out uint needed);
    [DllImport("user32.dll", SetLastError = true)]
    static extern bool EnumDesktopWindows(IntPtr desktop, EnumWindowsProc fn, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc fn, IntPtr lParam);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr hwnd, uint cmd);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h, ref POINT p);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
                                      IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetHandleInformation(IntPtr h, uint mask, uint flags);

    const uint GENERIC_ALL = 0x10000000;
    const int UOI_NAME = 2;
    const int STARTF_USESTDHANDLES = 0x100;
    const uint HANDLE_FLAG_INHERIT = 1;
    const uint CREATE_SUSPENDED = 0x4, CREATE_UNICODE_ENVIRONMENT = 0x400, CREATE_NO_WINDOW = 0x08000000;
    const uint GW_OWNER = 4;

    static void EnsureDesktop()
    {
        if (desktop_ != IntPtr.Zero) return;
        // Opens the desktop if this session has it already. Never switched to; it goes when the last
        // handle and the last thread on it do.
        desktop_ = CreateDesktopW(DesktopName, IntPtr.Zero, IntPtr.Zero, 0, GENERIC_ALL, IntPtr.Zero);
        if (desktop_ == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateDesktop");
    }

    // "WinSta0\KoggerChecks": the desktop in this process's window station.
    public static string FullName()
    {
        IntPtr station = GetProcessWindowStation();
        uint needed;
        GetUserObjectInformationW(station, UOI_NAME, IntPtr.Zero, 0, out needed);
        IntPtr buf = Marshal.AllocHGlobal((int)needed);
        try {
            if (!GetUserObjectInformationW(station, UOI_NAME, buf, needed, out needed))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "GetUserObjectInformation");
            return Marshal.PtrToStringUni(buf) + "\\" + DesktopName;
        } finally { Marshal.FreeHGlobal(buf); }
    }

    static FileStream InheritableFile(string path)
    {
        var f = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete);
        if (!SetHandleInformation(f.SafeFileHandle.DangerousGetHandle(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SetHandleInformation");
        return f;
    }

    // Starts file on the test desktop. environment: the whole block (DictionaryEntry items), or null for
    // this process's. stdOut / stdErr: files the output goes to, or empty (PowerShell passes $null to a
    // string parameter as ""). The process is made suspended and let go once .NET holds a handle to it,
    // so its exit code can be read however soon it ends.
    public static Process Start(string file, string arguments, string workingDirectory, IEnumerable environment,
                                string stdOut, string stdErr)
    {
        EnsureDesktop();
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.lpDesktop = FullName();
        FileStream outFile = null, errFile = null;
        bool redirect = !string.IsNullOrEmpty(stdOut) || !string.IsNullOrEmpty(stdErr);
        IntPtr env = IntPtr.Zero;
        PROCESS_INFORMATION pi;
        try {
            if (redirect) {
                si.dwFlags = STARTF_USESTDHANDLES;
                if (!string.IsNullOrEmpty(stdOut)) { outFile = InheritableFile(stdOut); si.hStdOutput = outFile.SafeFileHandle.DangerousGetHandle(); }
                if (!string.IsNullOrEmpty(stdErr)) { errFile = InheritableFile(stdErr); si.hStdError = errFile.SafeFileHandle.DangerousGetHandle(); }
            }
            if (environment != null) {
                var vars = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (object o in environment) {
                    var kv = (DictionaryEntry)o;
                    vars[(string)kv.Key] = kv.Value == null ? "" : kv.Value.ToString();
                }
                var block = new StringBuilder();
                foreach (var kv in vars) block.Append(kv.Key).Append('=').Append(kv.Value).Append('\0');
                block.Append('\0');
                env = Marshal.StringToHGlobalUni(block.ToString());
            }
            var cmd = new StringBuilder("\"" + file + "\"" + (string.IsNullOrEmpty(arguments) ? "" : " " + arguments));
            string app = Path.IsPathRooted(file) ? file : null;
            string cwd = string.IsNullOrEmpty(workingDirectory) ? null : workingDirectory;
            if (!CreateProcessW(app, cmd, IntPtr.Zero, IntPtr.Zero, redirect,
                                CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, cwd, ref si, out pi))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcess " + file);
        } finally {
            if (env != IntPtr.Zero) Marshal.FreeHGlobal(env);
            if (outFile != null) outFile.Dispose();
            if (errFile != null) errFile.Dispose();
        }
        try {
            Process p = Process.GetProcessById(pi.dwProcessId);
            IntPtr held = p.Handle;
            if (ResumeThread(pi.hThread) == 0xFFFFFFFF) throw new Win32Exception(Marshal.GetLastWin32Error(), "ResumeThread");
            return p;
        } catch {
            TerminateProcess(pi.hProcess, 1);
            throw;
        } finally {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    // The process's first visible top-level window without an owner on the test desktop, or zero.
    public static IntPtr MainWindow(int pid)
    {
        IntPtr found = IntPtr.Zero;
        if (desktop_ == IntPtr.Zero) return found;
        EnumWindowsProc fn = (h, l) => {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == (uint)pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == IntPtr.Zero) { found = h; return false; }
            return true;
        };
        EnumDesktopWindows(desktop_, fn, IntPtr.Zero);
        return found;
    }

    // Visible top-level windows of pid on the calling thread's desktop (the owner's, for a check script).
    public static int VisibleWindowsHere(int pid)
    {
        int n = 0;
        EnumWindowsProc fn = (h, l) => {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == (uint)pid && IsWindowVisible(h)) ++n;
            return true;
        };
        EnumWindows(fn, IntPtr.Zero);
        return n;
    }

    // The process the foreground window belongs to, 0 when there is none.
    public static int ForegroundPid()
    {
        IntPtr h = GetForegroundWindow();
        if (h == IntPtr.Zero) return 0;
        uint pid;
        GetWindowThreadProcessId(h, out pid);
        return (int)pid;
    }

    static void EnsureWorker()
    {
        if (worker_ != null) return;
        EnsureDesktop();
        var queue = new BlockingCollection<Action>();
        Exception failed = null;
        var ready = new ManualResetEventSlim();
        var t = new Thread(() => {
            // A thread can move to another desktop while it has no windows or hooks: a new one has none.
            if (!SetThreadDesktop(desktop_)) failed = new Win32Exception(Marshal.GetLastWin32Error(), "SetThreadDesktop");
            ready.Set();
            if (failed != null) return;
            foreach (Action a in queue.GetConsumingEnumerable()) a();
        });
        t.IsBackground = true;
        t.Start();
        ready.Wait();
        if (failed != null) throw failed;
        queue_ = queue;
        worker_ = t;
    }

    // Runs a static method on a thread of the test desktop: window messages and AttachThreadInput only
    // work between threads of one desktop.
    public static object Invoke(MethodInfo method, object[] args)
    {
        EnsureWorker();
        object result = null;
        Exception error = null;
        using (var done = new ManualResetEventSlim()) {
            queue_.Add(() => {
                try { result = method.Invoke(null, args); }
                catch (TargetInvocationException e) { error = e.InnerException; }
                catch (Exception e) { error = e; }
                finally { done.Set(); }
            });
            done.Wait();
        }
        if (error != null) throw error;
        return result;
    }

    // Wheel notches (120 a notch, the sign the direction) at a client point of the window, gapMs apart.
    // Run through Invoke.
    public static void Wheel(IntPtr window, int x, int y, int notches, int gapMs)
    {
        var p = new POINT();
        p.X = x; p.Y = y;
        ClientToScreen(window, ref p);
        var l = new IntPtr((p.Y << 16) | (p.X & 0xFFFF));
        int notch = notches > 0 ? 120 : -120;
        for (int n = 0; n < Math.Abs(notches); ++n) {
            PostMessage(window, 0x020A, new IntPtr((notch & 0xFFFF) << 16), l);
            Thread.Sleep(gapMs);
        }
    }
}
'@
}

<#
.SYNOPSIS
    [System.Diagnostics.Process]::Start($psi) on the test desktop.
.DESCRIPTION
    Takes the FileName, Arguments, WorkingDirectory and EnvironmentVariables of the start info (the
    environment as the start info has it: this process's, with the script's changes). No redirection
    through the start info: -StdOut / -StdErr give files instead.
.OUTPUTS
    System.Diagnostics.Process: WaitForExit, Kill and ExitCode work as for one Process.Start made.
#>
function Start-TestDesktopProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [System.Diagnostics.ProcessStartInfo] $StartInfo,
        [string] $StdOut,
        [string] $StdErr
    )
    if ($StartInfo.UseShellExecute -or $StartInfo.RedirectStandardOutput -or $StartInfo.RedirectStandardError -or $StartInfo.RedirectStandardInput) {
        throw 'Start-TestDesktopProcess: a start info with shell execute or redirection is not supported'
    }
    [KoggerTestDesktop]::Start($StartInfo.FileName, $StartInfo.Arguments, $StartInfo.WorkingDirectory,
                               $StartInfo.EnvironmentVariables, $StdOut, $StdErr)
}

<#
.SYNOPSIS
    The app's main window on the test desktop (IntPtr.Zero until it has one).
.DESCRIPTION
    Process.MainWindowHandle looks on the caller's desktop only and finds nothing there. -TimeoutMs: how long
    to wait for the window to come up.
#>
function Get-TestDesktopWindow {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [System.Diagnostics.Process] $Process,
        [int] $TimeoutMs = 0
    )
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    do {
        $h = [KoggerTestDesktop]::MainWindow($Process.Id)
        if ($h -ne [System.IntPtr]::Zero -or $Process.HasExited) { return $h }
        Start-Sleep -Milliseconds 50
    } while ($clock.ElapsedMilliseconds -lt $TimeoutMs)
    return $h
}

<#
.SYNOPSIS
    A static method (from Add-Type) run on a thread of the test desktop, for messages to the app's window.
.EXAMPLE
    Invoke-OnTestDesktop ([KoggerBench.Win].GetMethod('CtrlDrag')) @($h, 360, 520, 370)
#>
function Invoke-OnTestDesktop {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [System.Reflection.MethodInfo] $Method,
        [object[]] $Arguments = @()
    )
    [KoggerTestDesktop]::Invoke($Method, $Arguments)
}

<#
.SYNOPSIS
    Wheel notches posted at a client point of a window on the test desktop (120 a notch; the sign is the
    direction), -GapMs apart.
#>
function Send-TestDesktopWheel {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [System.IntPtr] $Window,
        [int] $X = 360,
        [int] $Y = 360,
        [Parameter(Mandatory = $true)] [int] $Notches,
        [int] $GapMs = 150
    )
    $null = [KoggerTestDesktop]::Invoke([KoggerTestDesktop].GetMethod('Wheel'), @($Window, $X, $Y, $Notches, $GapMs))
}
