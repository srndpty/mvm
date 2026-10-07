using System;
using Mvm.Tests;

public sealed class TestDisplayPowerApi : IDisplayPowerApi {
    public string Mode = "Good";
    public int Sets;
    public int Clears;
    public int Closes;
    public int Unregisters;
    public Action<int> Changed;
    public IntPtr Create() { return Mode == "CreateFailure" ? IntPtr.Zero : new IntPtr(1); }
    public bool Set(IntPtr handle, int kind) { Sets++; return Mode != "SetFailure"; }
    public bool Clear(IntPtr handle, int kind) { Clears++; return Mode != "ClearFailure"; }
    public void Close(IntPtr handle) { Closes++; }
    public uint Register(Action<int> changed, out IntPtr registration) {
        Changed = changed;
        registration = Mode == "RegisterFailure" || Mode == "MissingRegistration" ? IntPtr.Zero : new IntPtr(2);
        if (Mode == "RegisterFailure") return 5;
        changed(Mode == "InitiallyOff" ? 0 : 1);
        return 0;
    }
    public uint Unregister(IntPtr registration) { Unregisters++; return Mode == "UnregisterFailure" ? 5U : 0U; }
}
