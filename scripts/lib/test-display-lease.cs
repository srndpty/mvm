using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

namespace Mvm.Tests {
    public interface IDisplayPowerApi {
        IntPtr Create();
        bool Set(IntPtr handle, int kind);
        bool Clear(IntPtr handle, int kind);
        void Close(IntPtr handle);
        uint Register(Action<int> changed, out IntPtr registration);
        uint Unregister(IntPtr registration);
    }

    public sealed class NativeDisplayPowerApi : IDisplayPowerApi {
        [StructLayout(LayoutKind.Explicit, Size = 32)]
        private struct ReasonContext {
            [FieldOffset(0)] public uint Version;
            [FieldOffset(4)] public uint Flags;
            [FieldOffset(8)] public IntPtr Reason;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct Subscriber { public IntPtr Callback; public IntPtr Context; }
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate uint PowerCallback(IntPtr context, uint kind, IntPtr setting);
        private PowerCallback callback;
        private GCHandle callbackRoot;
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr PowerCreateRequest(ref ReasonContext reason);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool PowerSetRequest(IntPtr handle, int kind);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool PowerClearRequest(IntPtr handle, int kind);
        [DllImport("kernel32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);
        [DllImport("powrprof.dll")]
        private static extern uint PowerSettingRegisterNotification(ref Guid setting, uint flags,
            ref Subscriber subscriber, out IntPtr registration);
        [DllImport("powrprof.dll")]
        private static extern uint PowerSettingUnregisterNotification(IntPtr registration);
        public IntPtr Create() {
            IntPtr text = Marshal.StringToHGlobalUni("mvm の GUI 試験で自動消灯とスリープを防ぐ");
            try {
                var reason = new ReasonContext { Version = 0, Flags = 1, Reason = text };
                return PowerCreateRequest(ref reason);
            } finally { Marshal.FreeHGlobal(text); }
        }
        public bool Set(IntPtr handle, int kind) { return PowerSetRequest(handle, kind); }
        public bool Clear(IntPtr handle, int kind) { return PowerClearRequest(handle, kind); }
        public void Close(IntPtr handle) { CloseHandle(handle); }
        public uint Register(Action<int> changed, out IntPtr registration) {
            var guid = new Guid("2b84c20e-ad23-4ddf-93db-05ffbd7efca5");
            callback = (context, kind, setting) => {
                if (kind == 0x8013 && setting != IntPtr.Zero &&
                    Marshal.ReadInt32(setting, 16) == 4)
                    changed(Marshal.ReadInt32(setting, 20));
                return 0;
            };
            var subscriber = new Subscriber {
                Callback = Marshal.GetFunctionPointerForDelegate(callback), Context = IntPtr.Zero
            };
            callbackRoot = GCHandle.Alloc(callback);
            uint result = PowerSettingRegisterNotification(ref guid, 2, ref subscriber, out registration);
            if (result != 0) callbackRoot.Free();
            return result;
        }
        public uint Unregister(IntPtr registration) {
            uint result = PowerSettingUnregisterNotification(registration);
            GC.KeepAlive(callback);
            if (result == 0 && callbackRoot.IsAllocated) callbackRoot.Free();
            return result;
        }
    }

    // 要求は process の handle に所有させる。PowerShell の実行 thread が変わっても保持される。
    public sealed class DisplayPowerLease : IDisposable {
        private readonly IDisplayPowerApi api;
        private readonly ManualResetEventSlim initialState = new ManualResetEventSlim(false);
        private IntPtr handle;
        private IntPtr registration;
        private bool displaySet;
        private bool systemSet;
        private int started;
        private int state = -1;
        private int invalid;
        private readonly ConcurrentQueue<string> observations = new ConcurrentQueue<string>();
        public int State { get { return Volatile.Read(ref state); } }
        public bool Invalid { get { return Volatile.Read(ref invalid) != 0; } }
        public string[] Observations { get { return observations.ToArray(); } }
        public DisplayPowerLease() : this(new NativeDisplayPowerApi()) { }
        public DisplayPowerLease(IDisplayPowerApi api) {
            this.api = api;
            try {
                handle = api.Create();
                if (handle == IntPtr.Zero || handle == new IntPtr(-1))
                    throw new InvalidOperationException("PROTOCOL_INVALID: 電源要求を作成できません");
                if (!(displaySet = api.Set(handle, 0)) || !(systemSet = api.Set(handle, 1)))
                    throw new InvalidOperationException("PROTOCOL_INVALID: 自動消灯とスリープを防げません");
                IntPtr candidate;
                uint result = api.Register(Observe, out candidate);
                if (result != 0 || candidate == IntPtr.Zero)
                    throw new InvalidOperationException("PROTOCOL_INVALID: 電源状態の通知を登録できません: " + result);
                registration = candidate;
                if (!initialState.Wait(5000) || State < 1 || State > 2)
                    throw new InvalidOperationException("PROTOCOL_INVALID: 描画先の電源状態を確認できません: " + State);
                Volatile.Write(ref started, 1);
                AssertValid();
            } catch { Dispose(); throw; }
        }
        private void Observe(int value) {
            observations.Enqueue(DateTime.UtcNow.ToString("o") + ": " + value);
            Volatile.Write(ref state, value);
            if (Volatile.Read(ref started) != 0 && (value < 1 || value > 2))
                Volatile.Write(ref invalid, 1);
            initialState.Set();
        }
        public void AssertValid() {
            if (Invalid || State < 1 || State > 2)
                throw new InvalidOperationException("PROTOCOL_INVALID: 実行中に描画先の電源状態が不成立になりました: " + String.Join(", ", Observations));
        }
        public void Dispose() {
            var errors = new List<string>();
            if (registration != IntPtr.Zero) {
                if (api.Unregister(registration) != 0) errors.Add("通知の解除");
                registration = IntPtr.Zero;
            }
            if (handle != IntPtr.Zero && handle != new IntPtr(-1)) {
                if (systemSet && !api.Clear(handle, 1)) errors.Add("スリープ防止の解除");
                if (displaySet && !api.Clear(handle, 0)) errors.Add("自動消灯防止の解除");
                api.Close(handle);
                handle = IntPtr.Zero;
            }
            if (errors.Count != 0) {
                Volatile.Write(ref invalid, 1);
                throw new InvalidOperationException("PROTOCOL_INVALID: 電源前提の解放に失敗しました: " + String.Join(", ", errors));
            }
        }
    }
}
