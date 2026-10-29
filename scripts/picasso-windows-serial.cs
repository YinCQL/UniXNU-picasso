// Windows host capture only. This code never sends a device console command.
// C# 5 / .NET Framework 4.5 compatible for Windows PowerShell 5.1 Add-Type.
using System;
using System.ComponentModel;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace PicassoCapture
{
    public sealed class SerialStageException : Win32Exception
    {
        public string Stage { get; private set; }
        public SerialStageException(string stage, int error)
            : base(error, stage + " failed (Win32 " + error + "): " + new Win32Exception(error).Message)
        {
            Stage = stage;
        }
    }

    public sealed class NativeSerial : IDisposable
    {
        private const uint GenericRead = 0x80000000;
        private const uint GenericWrite = 0x40000000;
        private const uint OpenExisting = 3;
        private const uint FileFlagOverlapped = 0x40000000;
        private const int ErrorIoPending = 997;
        private const int ErrorOperationAborted = 995;
        private const int ErrorNotFound = 1168;
        private const int ErrorSemaphoreTimeout = 121;
        private SafeFileHandle handle;
        private bool disposed;
        private readonly object gate = new object();
        private static volatile string lastStage = "idle";
        public static string LastStage { get { return lastStage; } }
        public string PortName { get; private set; }

        [StructLayout(LayoutKind.Sequential)]
        private struct CommTimeouts
        {
            public uint ReadIntervalTimeout, ReadTotalTimeoutMultiplier, ReadTotalTimeoutConstant;
            public uint WriteTotalTimeoutMultiplier, WriteTotalTimeoutConstant;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Dcb
        {
            public uint Length, BaudRate, Flags;
            public ushort Reserved, XonLim, XoffLim;
            public byte ByteSize, Parity, StopBits;
            public sbyte XonChar, XoffChar, ErrorChar, EofChar, EvtChar;
            public ushort Reserved1;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct OverlappedData
        {
            public IntPtr Internal, InternalHigh;
            public uint Offset, OffsetHigh;
            public IntPtr Event;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
            IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetCommTimeouts(SafeFileHandle file, ref CommTimeouts timeouts);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetCommState(SafeFileHandle file, ref Dcb dcb);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetCommState(SafeFileHandle file, ref Dcb dcb);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool EscapeCommFunction(SafeFileHandle file, uint function);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ReadFile(SafeFileHandle file, IntPtr data, uint length,
            out uint read, IntPtr overlapped);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetOverlappedResult(SafeFileHandle file, IntPtr overlapped,
            out uint transferred, bool wait);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CancelIoEx(SafeFileHandle file, IntPtr overlapped);

        // Default deliberately avoids line-coding / modem-control requests.
        // The Picasso bulk-IN pump uses configured state, not DTR, to send logs.
        // This does not fix an unresponsive gadget; failures retain their stage.
        public NativeSerial(string port) : this(port, false) { }

        public NativeSerial(string port, bool configureLine)
        {
            if (String.IsNullOrEmpty(port) || !System.Text.RegularExpressions.Regex.IsMatch(
                port, "^COM[1-9][0-9]*$", System.Text.RegularExpressions.RegexOptions.IgnoreCase))
                throw new ArgumentException("Expected a COM port name such as COM8", "port");
            PortName = port.ToUpperInvariant();
            try
            {
                lastStage = "CreateFile(" + PortName + ")";
                handle = CreateFile(@"\\.\" + PortName, GenericRead | GenericWrite, 0,
                    IntPtr.Zero, OpenExisting, FileFlagOverlapped, IntPtr.Zero);
                if (handle.IsInvalid) ThrowLastError(lastStage);

                lastStage = "SetCommTimeouts(" + PortName + ")";
                CommTimeouts timeouts = new CommTimeouts();
                // Return available data immediately, otherwise wait up to 250ms
                // for the first byte. The overlapped wait also has a deadline.
                timeouts.ReadIntervalTimeout = UInt32.MaxValue;
                timeouts.ReadTotalTimeoutMultiplier = UInt32.MaxValue;
                timeouts.ReadTotalTimeoutConstant = 250;
                timeouts.WriteTotalTimeoutConstant = 1000;
                if (!SetCommTimeouts(handle, ref timeouts)) ThrowLastError(lastStage);

                if (configureLine) ConfigureLine();
                lastStage = "ready(" + PortName + ")";
            }
            catch
            {
                if (handle != null) handle.Dispose();
                throw;
            }
        }

        private void ConfigureLine()
        {
            lastStage = "GetCommState(" + PortName + ")";
            Dcb dcb = new Dcb();
            dcb.Length = (uint)Marshal.SizeOf(typeof(Dcb));
            if (!GetCommState(handle, ref dcb)) ThrowLastError(lastStage);
            dcb.BaudRate = 115200;
            dcb.ByteSize = 8;
            dcb.Parity = 0;
            dcb.StopBits = 0;
            // Binary, no flow control, DTR/RTS enabled. Preserve reserved flags.
            dcb.Flags = (dcb.Flags & 0xFFFF8000u) | 1u | (1u << 4) | (1u << 12);
            lastStage = "SetCommState(115200 8N1, DTR/RTS enabled, " + PortName + ")";
            if (!SetCommState(handle, ref dcb)) ThrowLastError(lastStage);
        }

        // Explicit diagnostics only; default capture does not call this.
        public void SetControlLines(bool dtr, bool rts)
        {
            lock (gate)
            {
                EnsureOpen();
                lastStage = "EscapeCommFunction(" + (dtr ? "SETDTR" : "CLRDTR") + ", " + PortName + ")";
                if (!EscapeCommFunction(handle, dtr ? 5u : 6u)) ThrowLastError(lastStage);
                lastStage = "EscapeCommFunction(" + (rts ? "SETRTS" : "CLRRTS") + ", " + PortName + ")";
                if (!EscapeCommFunction(handle, rts ? 3u : 4u)) ThrowLastError(lastStage);
            }
        }

        private sealed class ReadRequest : IDisposable
        {
            public readonly IntPtr Buffer;
            public readonly IntPtr Overlapped;
            public readonly ManualResetEvent Ready;
            private bool released;
            public ReadRequest(int length)
            {
                Ready = new ManualResetEvent(false);
                Buffer = Marshal.AllocHGlobal(length);
                Overlapped = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OverlappedData)));
                OverlappedData state = new OverlappedData();
                state.Event = Ready.SafeWaitHandle.DangerousGetHandle();
                Marshal.StructureToPtr(state, Overlapped, false);
            }
            public void Dispose()
            {
                if (released) return;
                released = true;
                Marshal.FreeHGlobal(Overlapped);
                Marshal.FreeHGlobal(Buffer);
                Ready.Dispose();
            }
            // A broken driver can fail to complete cancellation promptly. Keep
            // its unmanaged buffers alive until I/O really finishes, even after
            // the file handle is closed, rather than free memory owned by I/O.
            public void ReleaseAfterCompletion()
            {
                ThreadPool.QueueUserWorkItem(delegate(object unused)
                {
                    Ready.WaitOne();
                    Dispose();
                });
            }
        }

        public int Read(byte[] destination, int timeoutMilliseconds)
        {
            if (destination == null) throw new ArgumentNullException("destination");
            if (destination.Length == 0) return 0;
            if (timeoutMilliseconds < 1) throw new ArgumentOutOfRangeException("timeoutMilliseconds");
            lock (gate)
            {
                EnsureOpen();
                ReadRequest request = new ReadRequest(destination.Length);
                bool deferredRelease = false;
                try
                {
                    lastStage = "ReadFile(" + PortName + ")";
                    uint count;
                    bool completed = ReadFile(handle, request.Buffer, (uint)destination.Length,
                        out count, request.Overlapped);
                    if (!completed)
                    {
                        int error = Marshal.GetLastWin32Error();
                        if (error != ErrorIoPending) throw new SerialStageException(lastStage, error);
                        if (!request.Ready.WaitOne(timeoutMilliseconds))
                        {
                            lastStage = "CancelIoEx(read deadline, " + PortName + ")";
                            if (!CancelIoEx(handle, request.Overlapped))
                            {
                                error = Marshal.GetLastWin32Error();
                                if (error != ErrorNotFound)
                                {
                                    handle.Dispose();
                                    deferredRelease = true;
                                    request.ReleaseAfterCompletion();
                                    throw new SerialStageException(lastStage, error);
                                }
                            }
                            if (!request.Ready.WaitOne(1000))
                            {
                                handle.Dispose();
                                deferredRelease = true;
                                request.ReleaseAfterCompletion();
                                throw new SerialStageException("CancelIoEx completion(" + PortName + ")", ErrorSemaphoreTimeout);
                            }
                        }
                    }
                    if (!GetOverlappedResult(handle, request.Overlapped, out count, false))
                    {
                        int error = Marshal.GetLastWin32Error();
                        if (error == ErrorOperationAborted) return 0;
                        throw new SerialStageException("GetOverlappedResult(" + PortName + ")", error);
                    }
                    if (count > destination.Length) throw new IOException("Serial driver returned an invalid byte count");
                    if (count != 0) Marshal.Copy(request.Buffer, destination, 0, (int)count);
                    return (int)count;
                }
                finally
                {
                    if (!deferredRelease) request.Dispose();
                }
            }
        }

        private static void ThrowLastError(string stage)
        {
            throw new SerialStageException(stage, Marshal.GetLastWin32Error());
        }
        private void EnsureOpen()
        {
            if (disposed || handle == null || handle.IsClosed || handle.IsInvalid)
                throw new ObjectDisposedException("NativeSerial");
        }
        public void Dispose()
        {
            lock (gate)
            {
                if (disposed) return;
                disposed = true;
                if (handle != null) handle.Dispose();
            }
        }
    }

    public sealed class DurableLog : IDisposable
    {
        private readonly object gate = new object();
        private FileStream stream;
        private long bytesWritten;
        public string Path { get; private set; }
        public long BytesWritten { get { lock (gate) { return bytesWritten; } } }
        public DurableLog(string path)
        {
            Path = System.IO.Path.GetFullPath(path);
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path));
            stream = new FileStream(Path, FileMode.Append, FileAccess.Write, FileShare.Read,
                65536, FileOptions.WriteThrough);
        }
        public void Write(byte[] buffer, int count)
        {
            if (buffer == null) throw new ArgumentNullException("buffer");
            if (count < 0 || count > buffer.Length) throw new ArgumentOutOfRangeException("count");
            lock (gate)
            {
                if (stream == null) throw new ObjectDisposedException("DurableLog");
                if (count == 0) return;
                stream.Write(buffer, 0, count);
                stream.Flush(true);
                bytesWritten += count;
            }
        }
        public void Flush()
        {
            lock (gate) { if (stream != null) stream.Flush(true); }
        }
        public void Dispose()
        {
            lock (gate)
            {
                if (stream == null) return;
                try { stream.Flush(true); }
                finally { stream.Dispose(); stream = null; }
            }
        }
    }

    // Screen output is expendable; it must never hold up serial capture or disk
    // writes. A full/blocked preview drops only its own copy of received bytes.
    public sealed class PreviewWriter : IDisposable
    {
        private readonly object gate = new object();
        private readonly Queue<byte[]> queue = new Queue<byte[]>();
        private readonly Stream output;
        private readonly int capacity;
        private readonly Thread worker;
        private int pendingBytes;
        private long droppedBytes;
        private bool stopping;
        private volatile string lastError;
        public long BytesDropped { get { return Interlocked.Read(ref droppedBytes); } }
        public string LastError { get { return lastError; } }
        public int PendingBytes { get { lock (gate) { return pendingBytes; } } }

        public PreviewWriter() : this(Console.OpenStandardOutput(), 1048576) { }
        public PreviewWriter(Stream target, int maxQueuedBytes)
        {
            if (target == null) throw new ArgumentNullException("target");
            if (maxQueuedBytes < 1) throw new ArgumentOutOfRangeException("maxQueuedBytes");
            output = target;
            capacity = maxQueuedBytes;
            worker = new Thread(Run);
            worker.IsBackground = true;
            worker.Name = "Picasso console preview";
            worker.Start();
        }
        public bool Enqueue(byte[] source, int count)
        {
            if (source == null) throw new ArgumentNullException("source");
            if (count < 0 || count > source.Length) throw new ArgumentOutOfRangeException("count");
            if (count == 0) return true;
            lock (gate)
            {
                if (stopping || count > capacity - pendingBytes)
                {
                    Interlocked.Add(ref droppedBytes, count);
                    return false;
                }
                byte[] copy = new byte[count];
                Buffer.BlockCopy(source, 0, copy, 0, count);
                queue.Enqueue(copy);
                pendingBytes += count;
                Monitor.Pulse(gate);
                return true;
            }
        }
        private void Run()
        {
            try
            {
                while (true)
                {
                    byte[] next;
                    lock (gate)
                    {
                        while (!stopping && queue.Count == 0) Monitor.Wait(gate);
                        if (stopping) return;
                        next = queue.Dequeue();
                        pendingBytes -= next.Length;
                    }
                    output.Write(next, 0, next.Length);
                    output.Flush();
                }
            }
            catch (Exception error)
            {
                lastError = error.Message;
                StopQueue();
            }
        }
        private void StopQueue()
        {
            lock (gate)
            {
                stopping = true;
                Interlocked.Add(ref droppedBytes, pendingBytes);
                queue.Clear();
                pendingBytes = 0;
                Monitor.PulseAll(gate);
            }
        }
        public void Dispose()
        {
            StopQueue();
            // Console QuickEdit, a full pipe, or a closed terminal must not hold
            // capture shutdown hostage. Never close the process stdout stream.
            worker.Join(150);
        }
    }

    public sealed class CloseGuard : IDisposable
    {
        private delegate bool ConsoleHandler(uint control);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetConsoleCtrlHandler(ConsoleHandler handler, bool add);
        private readonly ConsoleHandler handler;
        private readonly DurableLog log;
        private int stopRequested;
        private bool disposed;
        public bool StopRequested { get { return Interlocked.CompareExchange(ref stopRequested, 0, 0) != 0; } }
        public string LastFlushError { get; private set; }
        public CloseGuard(DurableLog durableLog)
        {
            if (durableLog == null) throw new ArgumentNullException("durableLog");
            log = durableLog;
            handler = OnControl;
            if (!SetConsoleCtrlHandler(handler, true))
                throw new SerialStageException("SetConsoleCtrlHandler", Marshal.GetLastWin32Error());
        }
        private bool OnControl(uint control)
        {
            if (control != 0 && control != 1 && control != 2 && control != 5 && control != 6) return false;
            Interlocked.Exchange(ref stopRequested, 1);
            try { log.Flush(); }
            catch (Exception error) { LastFlushError = error.Message; }
            return true;
        }
        public void RequestStop()
        {
            Interlocked.Exchange(ref stopRequested, 1);
            log.Flush();
        }
        public void Dispose()
        {
            if (disposed) return;
            disposed = true;
            SetConsoleCtrlHandler(handler, false);
        }
    }
}
