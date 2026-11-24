using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using AVOPSEMU.Wpf.Interop;
using AVOPSEMU.Wpf.Library;

internal static class InteropSmokeTests
{
    private static int Main()
    {
        var tests = new (string Name, Action Run)[]
        {
            ("managed x64 ABI struct sizes and offsets", TestManagedLayouts),
            ("DLL load from WPF output and ABI/version/features", TestDllLoadAndAbi),
            ("WPF library import, persistence, and custom cover path", TestWpfGameLibrary),
            ("NativeSession SafeHandle lifecycle and single-session rule", TestSafeHandleLifecycle),
            ("512 KiB project BIOS load and BIOS-route disc boot", TestProjectBiosRoute),
            ("Runner start/pause/resume/stop and live CPU snapshot", TestRunnerLifecycleAndSnapshot),
            ("real RGBA framebuffer copy and dimensions", TestRgbaFramebuffer),
            ("HostPad press/release/focus-loss and audio status/mute", TestInputAndAudio),
            ("default host audio backend and nonfatal unavailable audio", TestHostAudioBackend),
            ("destroy running session and release native resources", TestDestroyWhileRunning),
            ("BIOS and disc native errors are surfaced safely", TestMediaErrors),
            ("UTF-8 path, native error text, and error-buffer sizing", TestUtf8ErrorBuffer),
            ("missing DLL and wrong-architecture diagnostics", TestLoadFailureDiagnostics)
        };

        foreach (var (name, run) in tests)
        {
            try
            {
                run();
                Console.WriteLine($"PASS: {name}");
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine($"FAIL: {name}{Environment.NewLine}{exception}");
                return 1;
            }
        }

        Console.WriteLine($"PASS: all {tests.Length} managed interop smoke tests");
        return 0;
    }

    private static void TestManagedLayouts()
    {
        Require(Environment.Is64BitProcess, "The interop tests must run as x64.");

        foreach (var report in NativeAbiValidator.ValidateManagedLayouts())
        {
            Console.WriteLine($"  {report}");
        }
    }

    private static void TestDllLoadAndAbi()
    {
        var info = NativeAbiValidator.Validate();
        var expectedDll = Path.GetFullPath(
            Path.Combine(AppContext.BaseDirectory, "avopsemu_capi.dll"));
        Require(File.Exists(expectedDll), $"Expected deployed DLL is missing: {expectedDll}");
        Require(
            string.Equals(NativeLibraryResolver.LoadedPath, expectedDll, StringComparison.OrdinalIgnoreCase),
            $"P/Invoke resolver used '{NativeLibraryResolver.LoadedPath}', expected '{expectedDll}'.");
        Require(info.AbiVersion == NativeConstants.AbiVersion, "Native ABI version differs.");
        Require(info.MinimumAbiVersion == NativeConstants.AbiVersion, "Native minimum ABI differs.");
        Require(info.CurrentAbiVersion == NativeConstants.AbiVersion, "Native current ABI differs.");
        Require(
            (info.FeatureBits & 0x1f) == 0x1f,
            $"Native feature bits were 0x{info.FeatureBits:X16}, expected all ABI v1 features.");

        Console.WriteLine($"  loaded={NativeLibraryResolver.LoadedPath}");
        Console.WriteLine(
            $"  ABI=0x{info.AbiVersion:X8}, minimum=0x{info.MinimumAbiVersion:X8}, " +
            $"current=0x{info.CurrentAbiVersion:X8}, features=0x{info.FeatureBits:X16}");
    }

    private static void TestSafeHandleLifecycle()
    {
        var session = NativeSession.Create(disableAudio: true);
        var state = session.GetState();
        Require(state.RunState == NativeConstants.StateCreated, "New native session is not CREATED.");
        Require(state.Flags == 0, "New native session unexpectedly has resource flags.");

        var secondCreationRejected = false;
        try
        {
            using var second = NativeSession.Create(disableAudio: true);
        }
        catch (NativeCallException exception)
        {
            secondCreationRejected = exception.Status == NativeConstants.StatusSessionLimit;
        }

        Require(secondCreationRejected, "Native ABI v1 did not reject a second live session.");
        session.Dispose();
        session.Dispose();
        Require(session.IsDisposed, "SafeHandle owner did not enter the disposed state.");
        RequireThrows<ObjectDisposedException>(() => session.GetState(),
            "Disposed NativeSession allowed a call through its handle.");

        using var replacement = NativeSession.Create(disableAudio: true);
        Require(replacement.GetState().RunState == NativeConstants.StateCreated,
            "SafeHandle disposal did not release the native single-session slot.");
    }

    private static void TestWpfGameLibrary()
    {
        var tempDirectory = Path.Combine(
            Path.GetTempPath(),
            $"avopsemu-wpf-library-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDirectory);
        var databasePath = Path.Combine(tempDirectory, "WpfGameLibrary.json");
        var gamePath = ProjectPath("examples", "homebrew.iso");
        var coverPath = ProjectPath("assets", "welcome.png");

        try
        {
            Require(File.Exists(gamePath), "The real project disc fixture is missing.");
            Require(File.Exists(coverPath), "The existing cover-image test asset is missing.");

            var store = new GameLibraryStore(databasePath);
            store.Load();
            Require(store.Entries.Count == 0, "A missing WPF library database did not load as empty.");

            var imported = store.AddGame(gamePath);
            Require(imported.Title == "homebrew", "Library title was not derived from the selected filename.");
            Require(ReferenceEquals(imported, store.AddGame(gamePath)),
                "Importing the same disc path did not return the existing library item.");
            Require(store.Entries.Count == 1, "Duplicate game selection created multiple library entries.");

            store.SetCover(gamePath, coverPath);
            store.Save();

            var reloaded = new GameLibraryStore(databasePath);
            reloaded.Load();
            Require(reloaded.Entries.Count == 1, "Saved game was not restored from the library database.");
            Require(reloaded.Entries[0].GamePath == Path.GetFullPath(gamePath),
                "Saved library game path did not round-trip.");
            Require(reloaded.Entries[0].CoverPath == Path.GetFullPath(coverPath),
                "Selected cover path did not round-trip.");

            Require(reloaded.RemoveGame(gamePath), "Library item could not be removed.");
            reloaded.Save();
            var finalLoad = new GameLibraryStore(databasePath);
            finalLoad.Load();
            Require(finalLoad.Entries.Count == 0, "Removed library item remained in the saved database.");
            Require(File.Exists(gamePath), "Removing a library item deleted the source game image.");

            var invalidImagePath = Path.Combine(tempDirectory, "not-an-image.txt");
            File.WriteAllText(invalidImagePath, "not an image");
            var newEntry = reloaded.AddGame(gamePath);
            var invalidCoverRejected = false;
            try
            {
                reloaded.SetCover(gamePath, invalidImagePath);
            }
            catch (InvalidDataException)
            {
                invalidCoverRejected = true;
            }

            Require(invalidCoverRejected, "Unsupported cover extension was accepted.");
            Require(newEntry.CoverPath.Length == 0, "Rejected cover selection modified the library item.");

            Console.WriteLine(
                $"  WPF library round-trip: {reloaded.Entries.Count} game(s), cover asset={Path.GetFileName(coverPath)}");
        }
        finally
        {
            Directory.Delete(tempDirectory, recursive: true);
        }
    }

    private static void TestProjectBiosRoute()
    {
        var biosPath = ProjectPath("examples", "boot_test_rom.bin");
        var cuePath = ProjectPath("examples", "catch_stars.cue");
        Require(File.Exists(biosPath), $"Project BIOS test fixture is missing: {biosPath}");
        Require(new FileInfo(biosPath).Length == 512 * 1024,
            "Project BIOS fixture is not exactly 512 KiB.");

        using var session = NativeSession.Create(disableAudio: true);
        session.LoadBios(biosPath);
        var state = session.GetState();
        Require(state.RunState == NativeConstants.StateStopped, "BIOS load did not leave session STOPPED.");
        Require((state.Flags & NativeConstants.SessionFlagBiosReady) != 0,
            "Native state did not report BIOS_READY after loading the fixture.");
        Require(state.Instructions == 0, "BIOS load did not reset the instruction counter.");

        session.MountDisc(cuePath);
        state = session.GetState();
        Require((state.Flags & NativeConstants.SessionFlagDiscMounted) != 0,
            "Native state did not report DISC_MOUNTED after a successful mount.");

        session.BootDisc(NativeConstants.BootRouteBios);
        state = session.GetState();
        Require((state.Flags & NativeConstants.SessionFlagProgramReady) != 0,
            "BIOS-route boot did not report PROGRAM_READY.");
        Require((state.Flags & NativeConstants.SessionFlagDirectExeBoot) == 0,
            "BIOS-route boot unexpectedly reported DIRECT_EXE_BOOT.");

        var startStatus = session.Start();
        Require(
            startStatus is NativeConstants.StatusOk or NativeConstants.StatusWarningAudioUnavailable,
            $"BIOS-fixture Runner start returned 0x{startStatus:X8}.");
        var running = WaitForInstructions(session, 1);
        var snapshot = session.GetCpuSnapshot();
        Require(running.RunState == NativeConstants.StateRunning && snapshot.Instructions > 0,
            "512 KiB project BIOS fixture did not execute through the BIOS route.");
        Console.WriteLine(
            $"  project BIOS test fixture: instructions={snapshot.Instructions}, PC=0x{snapshot.Pc:X8}");
        session.Stop();
        Require(session.GetState().RunState == NativeConstants.StateStopped,
            "Stopping the project BIOS-fixture session did not reach STOPPED.");
    }

    private static void TestRunnerLifecycleAndSnapshot()
    {
        using var session = CreateDirectExeSession(disableAudio: true);
        var startStatus = session.Start();
        Require(
            startStatus is NativeConstants.StatusOk or NativeConstants.StatusWarningAudioUnavailable,
            $"Native start returned unexpected status 0x{startStatus:X8}.");

        var running = WaitForInstructions(session, 1000);
        Require(running.RunState == NativeConstants.StateRunning,
            "Native state did not report RUNNING while Runner was active.");

        var snapshot = session.GetCpuSnapshot();
        Require(snapshot.Instructions >= 1000, "Live CPU snapshot did not advance.");
        Require(snapshot.Pc >= 0x80000000, "Live CPU snapshot PC is outside the cached MIPS region.");
        Require(snapshot.Gpr is { Length: 32 } && snapshot.Gpr[0] == 0,
            "CPU snapshot GPR layout/data is invalid.");
        Console.WriteLine(
            $"  live CPU snapshot: instructions={snapshot.Instructions}, PC=0x{snapshot.Pc:X8}");

        session.Pause();
        var paused = session.GetState();
        Require(paused.RunState == NativeConstants.StatePaused, "Pause did not reach native PAUSED state.");
        Thread.Sleep(20);
        Require(session.GetState().Instructions == paused.Instructions,
            "Instruction count changed while the native session was PAUSED.");

        startStatus = session.Start();
        Require(
            startStatus is NativeConstants.StatusOk or NativeConstants.StatusWarningAudioUnavailable,
            $"Native resume returned unexpected status 0x{startStatus:X8}.");
        var resumed = WaitForInstructions(session, paused.Instructions + 1);
        Require(resumed.RunState == NativeConstants.StateRunning,
            "Start did not resume the native PAUSED session.");

        session.Stop();
        Require(session.GetState().RunState == NativeConstants.StateStopped,
            "Stop did not reach native STOPPED state.");
    }

    private static void TestRgbaFramebuffer()
    {
        using var session = CreateDirectExeSession(disableAudio: true);
        _ = session.Start();
        _ = WaitForInstructions(session, 1000);

        var frame = session.CopyDisplayFrame();
        var info = frame.Info;
        Require(!frame.IsDisabled, "The real homebrew fixture unexpectedly disabled display output.");
        Require(info.PixelFormat == NativeConstants.PixelFormatRgba8,
            $"Native framebuffer format was {info.PixelFormat}, not RGBA8.");
        Require(info.Width > 0 && info.Height > 0, "Native framebuffer dimensions are empty.");
        Require(info.StrideBytes == info.Width * 4, "Native RGBA8 stride is not width * 4.");
        Require(info.RequiredBytes == (ulong)info.StrideBytes * info.Height,
            "Native framebuffer required_bytes does not match its dimensions.");
        Require((ulong)frame.RgbaBytes.Length == info.RequiredBytes,
            "Managed framebuffer array length does not match native required_bytes.");

        var nonBlackRgbBytes = 0;
        for (var index = 0; index < frame.RgbaBytes.Length; index += 4)
        {
            Require(frame.RgbaBytes[index + 3] == 0xff, "Native RGBA8 alpha was not opaque.");
            if (frame.RgbaBytes[index] != 0 ||
                frame.RgbaBytes[index + 1] != 0 ||
                frame.RgbaBytes[index + 2] != 0)
            {
                nonBlackRgbBytes++;
            }
        }

        Require(nonBlackRgbBytes > 0, "Native framebuffer contained no non-black RGB pixels.");
        Console.WriteLine(
            $"  copied framebuffer: {info.Width}x{info.Height}, {info.RequiredBytes} bytes, " +
            $"{info.VideoFrames} frames, non-black pixels={nonBlackRgbBytes}");
    }

    private static void TestInputAndAudio()
    {
        using var session = CreateDirectExeSession(disableAudio: true);

        const ushort crossPressed = 0xbfff; // PS1 button bit 14 is active-low Cross.
        session.SetPadButtons(crossPressed);
        var pressed = session.GetPadStatus();
        Require(pressed.ButtonsActiveLow == crossPressed,
            "Native HostPad did not report the active-low Cross press.");
        Require((pressed.PendingPresses & (1u << 14)) != 0,
            "Native HostPad did not publish the Cross press edge.");

        session.SetPadButtons(0xffff);
        var released = session.GetPadStatus();
        Require(released.ButtonsActiveLow == 0xffff,
            "Native HostPad did not report button release.");
        Require((released.PendingPresses & (1u << 14)) != 0,
            "Normal release incorrectly consumed the pending SIO press edge.");

        session.ReleasePad(); // Same native action used by WPF focus/deactivation handling.
        var focusReleased = session.GetPadStatus();
        Require(focusReleased.ButtonsActiveLow == 0xffff && focusReleased.PendingPresses == 0,
            "Focus-loss ReleasePad did not clear held state and pending edges.");

        var beforeMute = session.GetAudioStatus();
        Require((beforeMute.Flags & NativeConstants.AudioFlagDisabledByRequest) != 0,
            "Disabled-audio test session did not report DISABLED_BY_REQUEST.");
        session.SetAudioMuted(true);
        var muted = session.GetAudioStatus();
        Require((muted.Flags & NativeConstants.AudioFlagMuted) != 0,
            "Native audio status did not reflect mute.");
        session.SetAudioMuted(false);
        var afterUnmute = session.GetAudioStatus();
        Require((afterUnmute.Flags & NativeConstants.AudioFlagMuted) == 0,
            "Native audio status did not reflect unmute.");
        Console.WriteLine(
            $"  audio flags before/muted/unmuted: " +
            $"0x{beforeMute.Flags:X8}/0x{muted.Flags:X8}/0x{afterUnmute.Flags:X8}");

        var startStatus = session.Start();
        Require(startStatus == NativeConstants.StatusWarningAudioUnavailable,
            "Audio-disabled start did not return the documented nonfatal warning.");
        var running = WaitForInstructions(session, 1000);
        Require(running.RunState == NativeConstants.StateRunning && running.Instructions > 0,
            "CPU did not continue after the nonfatal audio-unavailable warning.");
        session.Stop();
    }

    private static void TestDestroyWhileRunning()
    {
        var session = CreateDirectExeSession(disableAudio: true);
        _ = session.Start();
        var running = WaitForInstructions(session, 1000);
        Require(running.RunState == NativeConstants.StateRunning, "Runner was not active before dispose.");

        session.Dispose(); // Native destroy joins Runner and closes audio through its shutdown path.
        session.Dispose();
        RequireThrows<ObjectDisposedException>(() => session.GetState(),
            "Disposed session remained callable after a running-session destroy.");

        using var replacement = NativeSession.Create(disableAudio: true);
        Require(replacement.GetState().RunState == NativeConstants.StateCreated,
            "Destroying a running session did not release the native session registry.");
    }

    private static void TestHostAudioBackend()
    {
        using var session = CreateDirectExeSession(disableAudio: false);
        var startStatus = session.Start();
        Require(
            startStatus is NativeConstants.StatusOk or NativeConstants.StatusWarningAudioUnavailable,
            $"Default-audio start returned unexpected status 0x{startStatus:X8}.");

        var running = WaitForInstructions(session, 1000);
        var audio = session.GetAudioStatus();
        Require(running.RunState == NativeConstants.StateRunning,
            "CPU stopped when the default host audio backend was unavailable.");
        Console.WriteLine(
            $"  host audio backend: {((audio.Flags & NativeConstants.AudioFlagAvailable) != 0 ? "available" : "unavailable")}, " +
            $"flags=0x{audio.Flags:X8}, nativeError=0x{audio.NativeError:X8}");
        session.Stop();
    }

    private static void TestMediaErrors()
    {
        using var session = NativeSession.Create(disableAudio: true);
        var invalidBiosPath = Path.Combine(Path.GetTempPath(), $"missing-bios-{Guid.NewGuid():N}.bin");
        var missingBios = CaptureNativeError(() => session.LoadBios(invalidBiosPath));
        Require(missingBios.Status == NativeConstants.StatusIoError,
            $"Missing BIOS returned status 0x{missingBios.Status:X8}, expected I/O error.");
        Require(!string.IsNullOrWhiteSpace(missingBios.Message),
            "Missing BIOS produced no user-facing native error.");

        var wrongSizeBios = CaptureNativeError(
            () => session.LoadBios(ProjectPath("examples", "homebrew.iso")));
        Require(wrongSizeBios.Status == NativeConstants.StatusFormatError,
            "Wrong-size BIOS fixture did not report FORMAT_ERROR.");

        var missingDisc = CaptureNativeError(
            () => session.MountDisc(Path.Combine(Path.GetTempPath(), $"missing-disc-{Guid.NewGuid():N}.cue")));
        Require(missingDisc.Status == NativeConstants.StatusIoError,
            "Missing disc did not report IO_ERROR.");
        Require(!string.IsNullOrWhiteSpace(missingDisc.Message),
            "Missing disc produced no user-facing native error.");
    }

    private static void TestUtf8ErrorBuffer()
    {
        var tempDirectory = Path.Combine(
            Path.GetTempPath(),
            $"avopsemu-interop-{Guid.NewGuid():N}-日本語");
        Directory.CreateDirectory(tempDirectory);

        using var error = new Utf8ErrorBuffer();
        ulong handle = 0;
        var destroyed = false;

        try
        {
            var cuePath = Path.Combine(tempDirectory, "検証ディスク.cue");
            const string missingTrack = "不存在-🎮.bin";
            File.WriteAllText(
                cuePath,
                $"FILE \"{missingTrack}\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n",
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

            var createInfo = NewCreateInfo();
            var status = NativeMethods.SessionCreate(ref createInfo, out handle, ref error.Native);
            RequireStatus(status, NativeConstants.StatusOk, "create UTF-8 test session", error);

            status = NativeMethods.SessionMountDisc(handle, cuePath, ref error.Native);
            RequireStatus(status, NativeConstants.StatusFormatError, "mount Unicode CUE path", error);
            Require(error.IsNullTerminated, "Native error text did not fit a trailing NUL.");
            Require(
                error.Message.StartsWith("Disc track file missing or empty:", StringComparison.Ordinal),
                $"Unexpected native error message: '{error.Message}'.");
            Require(
                error.Message.Contains(missingTrack, StringComparison.Ordinal),
                $"UTF-8 path text was not preserved in the native error: '{error.Message}'.");
            Require(
                error.RequiredBytes == (ulong)Encoding.UTF8.GetByteCount(error.Message) + 1,
                "UTF-8 error required_bytes is not the encoded byte count plus NUL.");

            status = NativeMethods.SessionDestroy(handle, ref error.Native);
            RequireStatus(status, NativeConstants.StatusOk, "destroy UTF-8 test session", error);
            destroyed = true;
        }
        finally
        {
            if (handle != 0 && !destroyed)
            {
                _ = NativeMethods.SessionDestroy(handle, ref error.Native);
            }

            Directory.Delete(tempDirectory, recursive: true);
        }

        using var shortError = new Utf8ErrorBuffer(capacityBytes: 2);
        var incompatibleInfo = NewCreateInfo();
        incompatibleInfo.AbiVersion = 0x00020000;
        var failedHandle = ulong.MaxValue;
        var mismatch = NativeMethods.SessionCreate(
            ref incompatibleInfo,
            out failedHandle,
            ref shortError.Native);
        RequireStatus(mismatch, NativeConstants.StatusAbiMismatch, "ABI mismatch with short error buffer", shortError);
        Require(failedHandle == 0, "Failed create did not clear the output handle.");
        Require(shortError.RequiredBytes > 2, "Native did not report the full required error capacity.");
        Require(shortError.IsNullTerminated, "Short native error buffer was not NUL-terminated.");
        Require(shortError.Message.Length == 1, "Short native error was not truncated to capacity minus NUL.");
        Require(shortError.MessageWithTruncationMarker.EndsWith("[truncated]", StringComparison.Ordinal),
            "Managed error presentation did not mark truncated native text.");
    }

    private static void TestLoadFailureDiagnostics()
    {
        var testDirectory = Path.Combine(
            Path.GetTempPath(),
            $"avopsemu-load-errors-{Guid.NewGuid():N}");
        Directory.CreateDirectory(testDirectory);

        try
        {
            var missingPath = Path.Combine(testDirectory, "missing", "avopsemu_capi.dll");
            ExpectLoadFailure(
                missingPath,
                NativeLibraryLoadFailure.Missing,
                "missing DLL diagnostic");

            var x86SystemDll = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.Windows),
                "SysWOW64",
                "version.dll");
            Require(File.Exists(x86SystemDll), $"Expected x86 test DLL was not found: {x86SystemDll}");

            var wrongArchitecturePath = Path.Combine(testDirectory, "wrong-arch", "avopsemu_capi.dll");
            Directory.CreateDirectory(Path.GetDirectoryName(wrongArchitecturePath)!);
            File.Copy(x86SystemDll, wrongArchitecturePath);
            ExpectLoadFailure(
                wrongArchitecturePath,
                NativeLibraryLoadFailure.WrongArchitecture,
                "wrong-architecture diagnostic");
        }
        finally
        {
            Directory.Delete(testDirectory, recursive: true);
        }
    }

    private static void ExpectLoadFailure(
        string libraryPath,
        NativeLibraryLoadFailure expectedFailure,
        string description)
    {
        try
        {
            var handle = NativeLibraryResolver.LoadAbsolutePath(libraryPath);
            NativeLibrary.Free(handle);
            throw new InvalidOperationException($"{description} unexpectedly loaded.");
        }
        catch (NativeLibraryLoadException exception)
        {
            Require(exception.Failure == expectedFailure,
                $"{description} was categorized as {exception.Failure}, expected {expectedFailure}.");
            Require(exception is NativeInteropException,
                $"{description} would not be caught by the WPF startup error handler.");
            Require(exception.Message.Contains(Path.GetFullPath(libraryPath), StringComparison.OrdinalIgnoreCase),
                $"{description} did not report the absolute library path.");
            Require(!string.IsNullOrWhiteSpace(exception.Message), $"{description} had no user-facing message.");
        }
    }

    private static NativeCreateInfo NewCreateInfo()
    {
        return new NativeCreateInfo
        {
            StructSize = (uint)Marshal.SizeOf<NativeCreateInfo>(),
            AbiVersion = NativeConstants.AbiVersion,
            Flags = NativeConstants.CreateFlagDisableAudio,
            Reserved = 0
        };
    }

    private static NativeSessionInfo NewSessionInfo()
    {
        return new NativeSessionInfo
        {
            StructSize = (uint)Marshal.SizeOf<NativeSessionInfo>(),
            AbiVersion = NativeConstants.AbiVersion
        };
    }

    private static void RequireStatus(
        uint actual,
        uint expected,
        string operation,
        Utf8ErrorBuffer error)
    {
        if (actual != expected)
        {
            throw new InvalidOperationException(
                $"{operation} returned 0x{actual:X8}, expected 0x{expected:X8}. " +
                $"Native error: '{error.MessageWithTruncationMarker}'.");
        }
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private static NativeSession CreateDirectExeSession(bool disableAudio)
    {
        var session = NativeSession.Create(disableAudio);
        try
        {
            session.MountDisc(ProjectPath("examples", "homebrew.iso"));
            session.BootDisc(NativeConstants.BootRouteDirectExe);
            var state = session.GetState();
            Require((state.Flags & NativeConstants.SessionFlagDiscMounted) != 0,
                "Direct-EXE test setup did not mount media.");
            Require((state.Flags & NativeConstants.SessionFlagDirectExeBoot) != 0,
                "Direct-EXE test setup did not use the ABI direct route.");
            return session;
        }
        catch
        {
            session.Dispose();
            throw;
        }
    }

    private static NativeSessionInfo WaitForInstructions(NativeSession session, ulong target)
    {
        for (var attempt = 0; attempt < 200; attempt++)
        {
            var state = session.GetState();
            if (state.Instructions >= target)
            {
                return state;
            }

            if (state.RunState == NativeConstants.StateFaulted)
            {
                throw new InvalidOperationException(
                    $"Native Runner faulted before reaching {target} instructions; " +
                    $"last native error=0x{state.LastErrorCode:X8}.");
            }

            Thread.Sleep(2);
        }

        throw new InvalidOperationException(
            $"Native Runner did not reach {target} instructions within the smoke-test deadline.");
    }

    private static NativeCallException CaptureNativeError(Action operation)
    {
        try
        {
            operation();
        }
        catch (NativeCallException exception)
        {
            return exception;
        }

        throw new InvalidOperationException("Expected the native operation to report an error.");
    }

    private static string ProjectPath(params string[] parts)
    {
        var root = new DirectoryInfo(AppContext.BaseDirectory);
        for (var level = 0; level < 5; level++)
        {
            root = root.Parent ?? throw new InvalidOperationException(
                "Could not locate the repository root from the interop test output directory.");
        }

        return Path.Combine(new[] { root.FullName }.Concat(parts).ToArray());
    }

    private static void RequireThrows<TException>(Action action, string message)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException)
        {
            return;
        }

        throw new InvalidOperationException(message);
    }
}
